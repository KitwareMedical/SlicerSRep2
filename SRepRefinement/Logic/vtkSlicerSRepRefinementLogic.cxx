/*==============================================================================

  Program: 3D Slicer

  Portions (c) Copyright Brigham and Women's Hospital (BWH) All Rights Reserved.

  See COPYRIGHT.txt
  or http://www.slicer.org/copyright/copyright.txt for details.

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

==============================================================================*/

// SRepRefinement Logic includes
#include "vtkSlicerSRepRefinementLogic.h"
#include "vtkSlicerSRepLogic.h"

// MRML includes
#include <vtkMRMLScene.h>

// VTK includes
#include <vtkCurvatures.h>
#include <vtkDoubleArray.h>
#include <vtkImageData.h>
#include <vtkImageMagnitude.h>
#include <vtkImageStencil.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkIntArray.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPointLocator.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataToImageStencil.h>

// ITK includes
#include <itkApproximateSignedDistanceMapImageFilter.h>
#include <itkCovariantVector.h>
#include <itkGradientImageFilter.h>
#include <itkImage.h>
#include <itkVTKImageToImageFilter.h>

// STD includes
#include <array>
#include <cassert>
#include <cstdlib>
#include <tuple>
#include <vector>

#include "Private/newuoa.h"

using Bounds = std::array<double, 6>;
using Pixel = unsigned char;
using ImageType = itk::Image<Pixel, 3>;
using RealImage = itk::Image<float, 3>;
using VectorImage = itk::Image<itk::CovariantVector<float, 3>, 3>;
using SDFAndGradient = std::tuple<itk::SmartPointer<RealImage>, itk::SmartPointer<VectorImage>>;

namespace {

//---------------------------------------------------------------------------
double Clamp(double val, double min, double max) {
  return val < min ? min : (val > max ? max : val);
}

//---------------------------------------------------------------------------
size_t Pow(size_t val, size_t exp) {
  size_t ret = 1;
  for (size_t i = 0; i < exp; ++i) {
    ret *= val;
  }
  return ret;
}

//---------------------------------------------------------------------------
vtkSmartPointer<vtkMatrix4x4> CreateBoundsToImageCoordsTransform(const Bounds& bounds) {
  const double xRange = bounds[1] - bounds[0];
  const double yRange = bounds[3] - bounds[2];
  const double zRange = bounds[5] - bounds[4];

  const auto transfomedRanges = [&](){
    if (xRange >= yRange && xRange >= zRange) {
      return std::make_tuple(1.0, yRange / xRange, zRange / xRange);
    } else if (yRange >= xRange && yRange >= zRange) {
      return std::make_tuple(xRange / yRange, 1.0, zRange / yRange);
    } else { //zRange is the largest
      return std::make_tuple(xRange / zRange, yRange / zRange, 1.0);
    }
  }();
  const auto xRangeTrans = std::get<0>(transfomedRanges);
  const auto yRangeTrans = std::get<1>(transfomedRanges);
  const auto zRangeTrans = std::get<2>(transfomedRanges);

  const double xOriginTrans = 0.5 - xRangeTrans / 2;
  const double yOriginTrans = 0.5 - yRangeTrans / 2;
  const double zOriginTrans = 0.5 - zRangeTrans / 2;

  auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
  mat->Zero();

  // scale factors to unit cube
  mat->SetElement(0,0, xRangeTrans / xRange);
  mat->SetElement(1,1, yRangeTrans / yRange);
  mat->SetElement(2,2, zRangeTrans / zRange);

  // translate amount
  mat->SetElement(0,3, xOriginTrans - xRangeTrans * bounds[0] / xRange);
  mat->SetElement(1,3, yOriginTrans - yRangeTrans * bounds[2] / yRange);
  mat->SetElement(2,3, zOriginTrans - zRangeTrans * bounds[4] / zRange);

  // the bottom-right corner has to be 1 to multiply with another transform matrix
  mat->SetElement(3,3, 1.0);
  return mat;
}

//---------------------------------------------------------------------------
Bounds ComputePolyDataToImageDataNewBounds(const Bounds& bounds)
{
  const double range[3] = {
    bounds[1] - bounds[0], // The range of x coordinate
    bounds[3] - bounds[2],
    bounds[5] - bounds[4],
  };
  const double ratioYX = range[1] / range[0];
  const double ratioZX = range[2] / range[0];
  const double ratioZY = range[2] / range[1];

  const double newCenter[3] = {0.5, 0.5, 0.5};
  Bounds newBounds;
  // put the longest axis to [0,1], scale other coordinates accordingly
  if(range[0] >= range[1] && range[0] >= range[2])
  {
      newBounds[0] = 0.0;
      newBounds[1] = 1.0;
      newBounds[2] = newCenter[1] - 0.5 * ratioYX;
      newBounds[3] = newCenter[1] + 0.5 * ratioYX;
      newBounds[4] = newCenter[2] - 0.5 * ratioZX;
      newBounds[5] = newCenter[2] + 0.5 * ratioZX;
  }
  else if(range[1] >= range[0] && range[1] >= range[2])
  {
      newBounds[0] = newCenter[0] - 0.5 / ratioYX;
      newBounds[1] = newCenter[0] + 0.5 / ratioYX;
      newBounds[2] = 0.0;
      newBounds[3] = 1.0;
      newBounds[4] = newCenter[2] - 0.5 * ratioZY;
      newBounds[5] = newCenter[2] + 0.5 * ratioZY;
  }
  else if(range[2] >= range[0] && range[2] >= range[1])
  {
      newBounds[0] = newCenter[0] - 0.5 / ratioZX;
      newBounds[1] = newCenter[0] + 0.5 / ratioZX;
      newBounds[2] = newCenter[1] - 0.5 / ratioZY;
      newBounds[3] = newCenter[1] + 0.5 / ratioZY;
      newBounds[4] = 0.0;
      newBounds[5] = 1.0;
  }
  return newBounds;
}

//---------------------------------------------------------------------------
// bounds must be able to contain the bounds of the polydata
vtkSmartPointer<vtkImageData> ConvertPolyDataToImageData(vtkPolyData* polydata, const Bounds& bounds, const double voxelSpacing)
{
  if (!polydata) {
    throw std::invalid_argument("expected non null PolyData when converting PolyData to ImageData");
  }

  // 1. transform the mesh into unit cube
  const double range[3] = {
    bounds[1] - bounds[0], // The range of x coordinate
    bounds[3] - bounds[2],
    bounds[5] - bounds[4],
  };

  const auto newBounds = ComputePolyDataToImageDataNewBounds(bounds);

  double rangeTransMesh[3];
  rangeTransMesh[0] = newBounds[1] - newBounds[0];
  rangeTransMesh[1] = newBounds[3] - newBounds[2];
  rangeTransMesh[2] = newBounds[5] - newBounds[4];
  auto newPts = vtkSmartPointer<vtkPoints>::New();
  for(int i = 0; i < polydata->GetNumberOfPoints(); ++i)
  {
      double oldPt[3], newPt[3];
      polydata->GetPoint(i, oldPt);
      newPt[0] = rangeTransMesh[0] * (oldPt[0] - bounds[0]) / range[0] + newBounds[0];
      newPt[1] = rangeTransMesh[1] * (oldPt[1] - bounds[2]) / range[1] + newBounds[2];
      newPt[2] = rangeTransMesh[2] * (oldPt[2] - bounds[4]) / range[2] + newBounds[4];
      newPts->InsertPoint(i, newPt);
  }
  newPts->Modified();

  auto transMesh = vtkSmartPointer<vtkPolyData>::New();
  transMesh->SetPoints(newPts);
  transMesh->SetPolys(polydata->GetPolys());

  auto whiteImage = vtkSmartPointer<vtkImageData>::New();

  double spacing[3]; // desired volume spacing
  spacing[0] = voxelSpacing;
  spacing[1] = voxelSpacing;
  spacing[2] = voxelSpacing;
  whiteImage->SetSpacing(spacing);

  // compute dimensions
  int dim[3];
  for (int i = 0; i < 3; i++)
  {
      dim[i] = static_cast<int>(1 / spacing[i]);
  }
  whiteImage->SetDimensions(dim);
  whiteImage->SetExtent(0, dim[0] - 1, 0, dim[1] - 1, 0, dim[2] - 1);

  double origin[3];
  origin[0] = newBounds[0];
  origin[1] = newBounds[2];
  origin[2] = newBounds[4];

  whiteImage->SetOrigin(origin);
  whiteImage->AllocateScalars(VTK_UNSIGNED_CHAR,1);

  // fill the image with foreground voxels:
  constexpr unsigned char inval = 255;
  constexpr unsigned char outval = 0;
  vtkIdType count = whiteImage->GetNumberOfPoints();
  for (vtkIdType i = 0; i < count; ++i)
  {
      whiteImage->GetPointData()->GetScalars()->SetTuple1(i, inval);
  }

  // polygonal data --> image stencil:
  auto pol2stenc = vtkSmartPointer<vtkPolyDataToImageStencil>::New();
  pol2stenc->SetInputData(transMesh);
  pol2stenc->SetTolerance(0);
  double polOrigin[3] = {0.0, 0.0, 0.0};
  pol2stenc->SetOutputOrigin(polOrigin);
  pol2stenc->SetOutputSpacing(spacing);
  pol2stenc->SetOutputWholeExtent(whiteImage->GetExtent());
  pol2stenc->Update();

  // cut the corresponding white image and set the background:
  auto imgstenc = vtkSmartPointer<vtkImageStencil>::New();
  imgstenc->SetInputData(whiteImage);
  imgstenc->SetStencilConnection(pol2stenc->GetOutputPort());
  imgstenc->ReverseStencilOff();
  imgstenc->SetBackgroundValue(outval);
  imgstenc->Update();

  return imgstenc->GetOutput();
}

//---------------------------------------------------------------------------
itk::SmartPointer<RealImage> CreateApproximateSignedDistanceMap(vtkImageData* input)
{
  using ApproxSDFFilterType = itk::ApproximateSignedDistanceMapImageFilter<ImageType, RealImage>;

  auto magnitude = vtkSmartPointer<vtkImageMagnitude>::New();
  magnitude->SetInputData(input);
  magnitude->Update();

  auto filter = itk::VTKImageToImageFilter< ImageType >::New();
  filter->SetInput(magnitude->GetOutput());
  try {
    filter->Update();
    auto asdfFilter = ApproxSDFFilterType::New();
    asdfFilter->SetInput(filter->GetOutput());
    asdfFilter->SetInsideValue(255);
    asdfFilter->SetOutsideValue(0);
    asdfFilter->Update();

    return asdfFilter->GetOutput();
  } catch (itk::ExceptionObject& error) {
    std::stringstream ss;
    ss << "Error creating ApproximateSignedDistanceMap: " << error;
    error.SetDescription(ss.str().c_str());
    throw;
  }
}

//---------------------------------------------------------------------------
itk::SmartPointer<VectorImage> CreateGradientDistanceFilter(itk::SmartPointer<RealImage> image)
{
  using GradientFilterType = itk::GradientImageFilter<RealImage, float>;
  auto gradientFilter = GradientFilterType::New();
  gradientFilter->SetInput(image);

  try {
    gradientFilter->Update();
    return gradientFilter->GetOutput();
  } catch (itk::ExceptionObject& error) {
    std::stringstream ss;
    ss << "Error creating GradientDistanceFilter: " << error;
    error.SetDescription(ss.str().c_str());
    throw;
  }
}

//---------------------------------------------------------------------------
// bounds must be able to contain the bounds of the polydata
SDFAndGradient CreateAntiAliasSignedDistanceMap(vtkPolyData* polyData, const Bounds& bounds, double voxelSpacing)
{
  auto imageData = ConvertPolyDataToImageData(polyData, bounds, voxelSpacing);
  auto antiAliasedSDFImage = CreateApproximateSignedDistanceMap(imageData);
  auto gradDistFilter = CreateGradientDistanceFilter(antiAliasedSDFImage);

  return std::make_tuple(antiAliasedSDFImage, gradDistFilter);
}

//---------------------------------------------------------------------------
Bounds ComputeMasterBounds(vtkPolyData* polyData, const vtkEllipticalSRep& srep) {
  if (!polyData) {
    throw std::invalid_argument("Expected existing polydata for computing bounds");
  }

  Bounds srepBounds;
  vtkMRMLSRepNode::GetSRepBounds(srep, srepBounds.data());

  Bounds polyDataBounds;
  polyData->GetBounds(polyDataBounds.data());

  return Bounds{
    std::min(srepBounds[0], polyDataBounds[0]), std::max(srepBounds[1], polyDataBounds[1]),
    std::min(srepBounds[2], polyDataBounds[2]), std::max(srepBounds[3], polyDataBounds[3]),
    std::min(srepBounds[4], polyDataBounds[4]), std::max(srepBounds[5], polyDataBounds[5])
  };
}

/// Progress returned will be in range [0,1]
using ProgressCallbackFunction = std::function<void(double)>;

/// Class for doing the refinement. Do not use directly, call free function RefineSRep instead.
class Refiner {
public:
  //---------------------------------------------------------------------------
  Refiner(
    const vtkEllipticalSRep& srep,
    vtkPolyData* polyData,
    double initialRegionSize,
    double finalRegionSize,
    int maxIterations,
    int interpolationLevel,
    double L0Weight,
    double L1Weight,
    double L2Weight)
    : m_voxelSpacing(0.005)
    , m_polyData(polyData)
    , m_srep(srep.SmartClone())
    , m_masterBounds(ComputeMasterBounds(m_polyData, *m_srep))
    , m_sdfAndGradient(CreateAntiAliasSignedDistanceMap(m_polyData, m_masterBounds, m_voxelSpacing))
    , m_srepToImageCoordsTransform(CreateBoundsToImageCoordsTransform(m_masterBounds))
    , m_flattenedUpCoeff()
    , m_flattenedDownCoeff()
    , m_initialRegionSize(initialRegionSize)
    , m_finalRegionSize(finalRegionSize)
    , m_maxIterations(maxIterations)
    , m_interpolationLevel(interpolationLevel)
    , m_currentCoeff(nullptr)
    , m_srepLogic()
    , m_L0Weight(L0Weight)
    , m_L1Weight(L1Weight)
    , m_L2Weight(L2Weight)
    , m_iteration(0)
    , m_totalProgressIterations(2 * m_maxIterations + 2 * m_srep->GetNumberOfLines()) // up and down iterations + 2 * # crest points
    , m_progressCallback()
  {
    this->GetInitialCoefficients();
  }

  void SetProgressCallback(ProgressCallbackFunction f) {
    this->m_progressCallback = f;
  }

  //---------------------------------------------------------------------------
  /// WARNING: don't call this more than once
  vtkSmartPointer<vtkEllipticalSRep> Run() {
    if (!m_srep->IsEmpty()) {
      m_iteration = 0; ReportProgress();
      this->RefineSpokes(SpokeType::UpOrientation);
      m_iteration = 1 * m_maxIterations; ReportProgress();
      this->RefineSpokes(SpokeType::DownOrientation);
      m_iteration = 2 * m_maxIterations; ReportProgress();
      this->RefineSpokes(SpokeType::CrestOrientation);
      m_iteration = m_totalProgressIterations;
    }
    return m_srep;
  }

private:
  using SpokeType = vtkSRepSkeletalPoint::SpokeOrientation;
  using IndexType = vtkEllipticalSRep::IndexType;
  class MinNewouaHelper {
  public:
    using SpokeType = Refiner::SpokeType;

    MinNewouaHelper(Refiner& refiner, SpokeType spokeType)
      : m_refiner(refiner)
      , m_spokeType(spokeType)
    {}

    double operator()(double* coeff) {
      return m_refiner.EvaluateObjectiveFunction(coeff, m_spokeType);
    }
  private:
    Refiner& m_refiner;
    SpokeType m_spokeType;
  };
  friend class MinNewouaHelper;

  double m_voxelSpacing;
  vtkSmartPointer<vtkPolyData> m_polyData;
  vtkSmartPointer<vtkEllipticalSRep> m_srep;
  Bounds m_masterBounds;
  SDFAndGradient m_sdfAndGradient;
  vtkSmartPointer<vtkMatrix4x4> m_srepToImageCoordsTransform;
  std::vector<double> m_flattenedUpCoeff;
  std::vector<double> m_flattenedDownCoeff;
  double m_initialRegionSize;
  double m_finalRegionSize;
  int m_maxIterations;
  int m_interpolationLevel;
  std::vector<double>* m_currentCoeff;
  vtkNew<vtkSlicerSRepLogic> m_srepLogic;
  double m_L0Weight;
  double m_L1Weight;
  double m_L2Weight;
  int m_iteration;
  int m_totalProgressIterations;
  ProgressCallbackFunction m_progressCallback;

  //---------------------------------------------------------------------------
  void IncrementIteration() {
    ++m_iteration;
    ReportProgress();
  }

  //---------------------------------------------------------------------------
  void ReportProgress() {
    if (m_progressCallback) {
      // we go through max iterations 3 times (up, down, crest)
      m_progressCallback(static_cast<double>(m_iteration) / m_totalProgressIterations);
    }
  }

  //---------------------------------------------------------------------------
  void RefineSpokes(SpokeType spokeType) {
    if (spokeType == SpokeType::CrestOrientation) {
      RefineCrestSpokes();
    } else {
      RefineUpDownSpokes(spokeType);
    }
  }

  //---------------------------------------------------------------------------
  void OptimizeCrestSpokeLengths(const double stepSize, const size_t maxIter) {
    constexpr double epsilon = 1e-5;
    vtkNew<vtkImplicitPolyDataDistance> implicitPolyDataDistance;
    implicitPolyDataDistance->SetInput(m_polyData);

    vtkEllipticalSRep::ModifiedBlocker blocker(m_srep);
    for (IndexType l = 0; l < m_srep->GetNumberOfLines(); ++l) {
      for (IndexType s = 0; s < m_srep->GetNumberOfSteps(); ++s) {
        auto* skeletalPoint = m_srep->GetSkeletalPoint(l, s);
        if (skeletalPoint->IsCrest()) {
          IncrementIteration();
          // get spoke by reference so we can update
          auto& spoke = *skeletalPoint->GetCrestSpoke();
          double dist = implicitPolyDataDistance->FunctionValue(spoke.GetBoundaryPoint().AsArray().data());
          double oldDist = dist;
          double thisStepSize = stepSize;
          for (size_t i = 0; i < maxIter; ++i) {
            if (abs(dist) <= epsilon) {
              break;
            }

            if (dist > 0) {
              // if spoke is too long, shorten it
              spoke.SetRadius(spoke.GetRadius() - thisStepSize);
            } else {
              // if spoke is too short, make it larger
              spoke.SetRadius(spoke.GetRadius() + thisStepSize);
            }

            dist = implicitPolyDataDistance->FunctionValue(spoke.GetBoundaryPoint().AsArray().data());
            if (oldDist * dist < 0) {
              // changed from outside to inside (or vice versa), decay step size
              thisStepSize /= 10;
            }
            oldDist = dist;
          }
        }
      }
    }
  }

  //---------------------------------------------------------------------------
  void RefineCrestSpokes() {
    // it makes no real sense to have the initial region size for min_newuoa (not used here) be the step size for
    // this optimization, but it was how it was before.
    OptimizeCrestSpokeLengths(m_initialRegionSize, m_maxIterations);

    vtkNew<vtkCurvatures> curvaturesFilter;
    curvaturesFilter->SetInputData(m_polyData);
    curvaturesFilter->SetCurvatureTypeToMaximum();
    curvaturesFilter->Update();

    vtkSmartPointer<vtkDoubleArray> maxC =
      vtkDoubleArray::SafeDownCast(curvaturesFilter->GetOutput()->GetPointData()->GetArray("Maximum_Curvature"));

    if(!maxC) {
      throw std::runtime_error("Error getting max curvature");
    }

    curvaturesFilter->SetCurvatureTypeToMinimum();
    curvaturesFilter->Update();

    vtkSmartPointer<vtkDoubleArray> minC =
        vtkDoubleArray::SafeDownCast(curvaturesFilter->GetOutput()->GetPointData()->GetArray("Minimum_Curvature"));
    if(!minC) {
      throw std::runtime_error("Error getting min curvature");
    }

    vtkNew<vtkPointLocator> locator;
    locator->SetDataSet(m_polyData);
    locator->BuildLocator();

    for (IndexType l = 0; l < m_srep->GetNumberOfLines(); ++l) {
      for (IndexType s = 0; s < m_srep->GetNumberOfSteps(); ++s) {
        auto* skeletalPoint = m_srep->GetSkeletalPoint(l, s);
        if (skeletalPoint->IsCrest()) {
          IncrementIteration();
          auto& spoke = *skeletalPoint->GetCrestSpoke();
          const vtkIdType idNearest = locator->FindClosestPoint(spoke.GetBoundaryPoint().AsArray().data());
          const double curMax = maxC->GetValue(idNearest);
          const double curMin = minC->GetValue(idNearest);
          const double rCrest = 1 / (max(abs(curMax), abs(curMin)));
          const double rDiff = spoke.GetRadius() - rCrest;
          if (rDiff <= 0) {
            continue;
          }

          // move skeletal point of this crest outward by rDiff
          const auto unitDir = spoke.GetDirection().Unit();
          const auto skeletalPoint = spoke.GetSkeletalPoint();
          spoke.SetSkeletalPoint(srep::Point3d(
            spoke.GetSkeletalPoint()[0] + unitDir[0] * rDiff,
            spoke.GetSkeletalPoint()[1] + unitDir[1] * rDiff,
            spoke.GetSkeletalPoint()[2] + unitDir[2] * rDiff));
          spoke.SetRadius(rCrest);
        }
      }
    }
  }

  //---------------------------------------------------------------------------
  void RefineUpDownSpokes(SpokeType spokeType) {
    m_currentCoeff = spokeType == SpokeType::UpOrientation ? &m_flattenedUpCoeff : &m_flattenedDownCoeff;
    MinNewouaHelper helper(*this, spokeType);
    min_newuoa(static_cast<int>(m_currentCoeff->size()), m_currentCoeff->data(), helper, m_initialRegionSize, m_finalRegionSize, m_maxIterations);

    // note: only the "spokeType" spokes are refined
    auto refinedSRep = this->Refine(*m_srep, m_currentCoeff->data(), spokeType);

    if (m_srep->GetNumberOfLines() != refinedSRep->GetNumberOfLines()) {
      throw std::runtime_error("Error: expected equal number of lines "
        + std::to_string(m_srep->GetNumberOfLines()) + "!=" + std::to_string(refinedSRep->GetNumberOfLines()));
    }
    if (m_srep->GetNumberOfSteps() != refinedSRep->GetNumberOfSteps()) {
      throw std::runtime_error("Error: expected equal number of steps "
        + std::to_string(m_srep->GetNumberOfSteps()) + "!=" + std::to_string(refinedSRep->GetNumberOfSteps()));
    }

    for (IndexType l = 0; l < m_srep->GetNumberOfLines(); ++l) {
      for (IndexType s = 0; s < m_srep->GetNumberOfSteps(); ++s) {
        // shallow copy the spoke, but that is ok because refinedSRep will go away and m_srep will be sole owner
        m_srep->GetSkeletalPoint(l, s)->SetSpoke(spokeType, refinedSRep->GetSkeletalPoint(l, s)->GetSpoke(spokeType));
      }
    }
  }

  //---------------------------------------------------------------------------
  // this temporary srep is constructed to compute the cost function value
  // The original srep should not be changed by each iteration
  vtkSmartPointer<vtkEllipticalSRep> Refine(vtkEllipticalSRep& srep, double* coeff, SpokeType spokeType) {
    constexpr double tolerance = 1e-13;

    auto clone = srep.SmartClone();
    if (spokeType == SpokeType::UpOrientation || spokeType == SpokeType::DownOrientation) {
      size_t c = 0; //coeff index
      for (IndexType l = 0; l < clone->GetNumberOfLines(); ++l) {
        for (IndexType s = 0; s < clone->GetNumberOfSteps(); ++s) {
          auto& spoke = *(clone->GetSkeletalPoint(l, s)->GetSpoke(spokeType));
          const double oldRadius = spoke.GetRadius();
          const auto oldUnitDir = spoke.GetDirection().Unit();

          const srep::Vector3d newUnitDir(coeff[c], coeff[c+1], coeff[c+2]);
          c += 3;
          double newRadius = exp(coeff[c++]) * oldRadius;

          if ( abs(oldRadius - newRadius) >= tolerance
            || abs(oldUnitDir[0] - newUnitDir[0]) >= tolerance
            || abs(oldUnitDir[1] - newUnitDir[1]) >= tolerance
            || abs(oldUnitDir[2] - newUnitDir[2]) >= tolerance)
          {
            spoke.SetDirectionAndMagnitude(newUnitDir * newRadius);
          }
        }
      }
    } else {
      throw std::invalid_argument("Don't know how to refine spoke of type " + std::to_string(static_cast<int>(spokeType)));
    }

    return clone;
  }

  //---------------------------------------------------------------------------
  std::pair<double, double> ComputeDistanceSquaredAndNormalToImage(const vtkEllipticalSRep& srep, SpokeType spokeType) {
    double totalDistSquared = 0.0;
    double totalNormalPenalty = 0.0;

    for (IndexType l = 0; l < srep.GetNumberOfLines(); ++l) {
      for (IndexType s = 0; s < srep.GetNumberOfSteps(); ++s) {
        const auto& spoke = *(srep.GetSkeletalPoint(l, s)->GetSpoke(spokeType));
        const auto boundaryPoint = spoke.GetBoundaryPoint();

        // transform boundary to image coordinate system
        const double boundaryArray[4] = {boundaryPoint[0], boundaryPoint[1], boundaryPoint[2], 1};
        double transformedBoundaryArray[4];
        m_srepToImageCoordsTransform->MultiplyPoint(boundaryArray, transformedBoundaryArray);

        //convert image coordinate system of [0,1] to index into image

        const long maxIndex = std::lround(1 / m_voxelSpacing) - 1;

        const long x = Clamp(std::lround(transformedBoundaryArray[0] / m_voxelSpacing), 0, maxIndex);
        const long y = Clamp(std::lround(transformedBoundaryArray[1] / m_voxelSpacing), 0, maxIndex);
        const long z = Clamp(std::lround(transformedBoundaryArray[2] / m_voxelSpacing), 0, maxIndex);

        RealImage::IndexType pixelIndex = {{x,y,z}};
        const float dist = std::get<0>(m_sdfAndGradient)->GetPixel(pixelIndex);
        const double distSquared = static_cast<double>(dist) * dist;

        VectorImage::IndexType indexGrad;
        indexGrad[0] = x;
        indexGrad[1] = y;
        indexGrad[2] = z;

        VectorImage::PixelType grad = std::get<1>(m_sdfAndGradient)->GetPixel(indexGrad);
        double normalVector[3];
        normalVector[0] = static_cast<double>(grad[0]);
        normalVector[1] = static_cast<double>(grad[1]);
        normalVector[2] = static_cast<double>(grad[2]);
        // normalize the normal vector
        vtkMath::Normalize(normalVector);

        const auto spokeDirection = spoke.GetDirection().Unit().AsArray();
        const double dotProduct = vtkMath::Dot(normalVector, spokeDirection.data());

        // The normal match (aka 1-dotProduct) (between [0,1]) is scaled by the distance so that the overall term is comparable
        totalDistSquared += distSquared;
        totalNormalPenalty += distSquared * (1 - dotProduct);
      }
    }
    return std::make_pair(totalDistSquared, totalNormalPenalty);
  }

  //---------------------------------------------------------------------------
  void ComputeRSradDerivatives(
    const vtkEllipticalSRep& interpolatedSRep,
    SpokeType spokeType,
    IndexType line,
    IndexType step,
    srep::Vector3d& dxdu,
    srep::Vector3d& dSdu,
    double& drdu,
    srep::Vector3d& dxdv,
    srep::Vector3d& dSdv,
    double& drdv)
  {
    const auto density = Pow(2, m_interpolationLevel);
    const auto stepSize = 1.0 / density;
    const auto numLines = interpolatedSRep.GetNumberOfLines();
    const auto numSteps = interpolatedSRep.GetNumberOfSteps();

    // U direction
    {
      const auto prevLine = (numLines + line - 1) % numLines;
      const auto nextLine = (numLines + line + 1) % numLines;

      const auto& u1 = *interpolatedSRep.GetSkeletalPoint(prevLine, step)->GetSpoke(spokeType);
      const auto& u2 = *interpolatedSRep.GetSkeletalPoint(nextLine, step)->GetSpoke(spokeType);

      drdu = (u2.GetRadius() - u1.GetRadius()) / stepSize / 2;
      dxdu = (u2.GetDirection().Unit() - u1.GetDirection().Unit()) / stepSize / 2;
      dSdu = (u2.GetDirection() - u1.GetDirection()) / stepSize / 2;
    }

    // V direction
    {
      const auto prevStep = step == 0 ? 0 : step - 1;
      const auto nextStep = step == numSteps - 1 ? numSteps - 1 : step + 1;
      const auto divisor = prevStep == step || nextStep == step ? 1 : 2;

      const auto& v1 = *interpolatedSRep.GetSkeletalPoint(line, prevStep)->GetSpoke(spokeType);
      const auto& v2 = *interpolatedSRep.GetSkeletalPoint(line, nextStep)->GetSpoke(spokeType);

      drdv = (v2.GetRadius() - v1.GetRadius()) / stepSize / divisor;
      dxdv = (v2.GetDirection().Unit() - v1.GetDirection().Unit()) / stepSize / divisor;
      dSdv = (v2.GetDirection() - v1.GetDirection()) / stepSize / divisor;
    }
  }

  //---------------------------------------------------------------------------
  // Uses the interpolated SRep and m_interpolationLevel to know which spokes are primary
  double ComputeRSradPenalty(const vtkEllipticalSRep& interpolatedSRep, SpokeType spokeType) {
    if (interpolatedSRep.IsEmpty()) {
      return 0;
    }

    double penalty = 0.0;
    const auto density = Pow(2, m_interpolationLevel);

    const auto numLines = interpolatedSRep.GetNumberOfLines() / static_cast<IndexType>(density);
    const auto numSteps = interpolatedSRep.GetNumberOfSteps() / static_cast<IndexType>(density);

    srep::Vector3d dxdu;
    srep::Vector3d dSdu;
    double drdu;
    srep::Vector3d dxdv;
    srep::Vector3d dSdv;
    double drdv;

    for (IndexType i = 0; i < numLines; ++i) {
      const auto ii = i * density;
      for (IndexType j = 0; j < numSteps; ++j) {
        const auto jj = j * density;

        // u is line-to-line direction
        // v is step-to-step direction
        ComputeRSradDerivatives(interpolatedSRep, spokeType, ii, jj, dxdu, dSdu, drdu, dxdv, dSdv, drdv);

        const auto U = interpolatedSRep.GetSkeletalPoint(ii, jj)->GetSpoke(spokeType)->GetDirection().Unit();

        // 2. construct rSrad Matrix
        double UTU[3][3]; // UT*U - I
        UTU[0][0] = U[0] * U[0] - 1;
        UTU[0][1] = U[0] * U[1];
        UTU[0][2] = U[0] * U[2];
        UTU[1][0] = U[1] * U[0];
        UTU[1][1] = U[1] * U[1] -1;
        UTU[1][2] = U[1] * U[2];
        UTU[2][0] = U[2] * U[0];
        UTU[2][1] = U[2] * U[1];
        UTU[2][2] = U[2] * U[2] -1;

        // Notation in Han, Qiong's dissertation
        Eigen::MatrixXd Q(2,3);
        Q(0,0) = dxdu[0] * UTU[0][0] + dxdu[1] * UTU[1][0] + dxdu[2] * UTU[2][0];
        Q(0,1) = dxdu[0] * UTU[0][1] + dxdu[1] * UTU[1][1] + dxdu[2] * UTU[2][1];
        Q(0,2) = dxdu[0] * UTU[0][2] + dxdu[1] * UTU[1][2] + dxdu[2] * UTU[2][2];

        Q(1,0) = dxdv[0] * UTU[0][0] + dxdv[1] * UTU[1][0] + dxdv[2] * UTU[2][0];
        Q(1,1) = dxdv[0] * UTU[0][1] + dxdv[1] * UTU[1][1] + dxdv[2] * UTU[2][1];
        Q(1,2) = dxdv[0] * UTU[0][2] + dxdv[1] * UTU[1][2] + dxdv[2] * UTU[2][2];

        Eigen::MatrixXd leftSide(2,3), rightSide(3, 2);
        leftSide(0,0) = dSdu[0] - drdu * U[0];
        leftSide(0,1) = dSdu[1] - drdu * U[1];
        leftSide(0,2) = dSdu[2] - drdu * U[2];

        leftSide(1,0) = dSdv[0] - drdv * U[0];
        leftSide(1,1) = dSdv[1] - drdv * U[1];
        leftSide(1,2) = dSdv[2] - drdv * U[2];

        Eigen::Matrix2d QQT, QQT_inv;
        QQT = Q * Q.transpose();
        QQT_inv = QQT.inverse();

        rightSide = Q.transpose() * QQT_inv;

        Eigen::Matrix2d rSradMat;
        rSradMat = leftSide * rightSide;
        rSradMat.transposeInPlace();
        // 3. compute rSrad penalty
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> eigensolver(rSradMat);
        double maxEigen = eigensolver.eigenvalues()[1];

        penalty += std::max(0.0, maxEigen - 1);
      }
    }

    return penalty;
  }

  //---------------------------------------------------------------------------
  /// Evaluates the objective function.
  ///
  /// The objective function has 3 conditions, L0, L1, and L2
  ///
  /// L0 - measures the overall squared distance from the tips of the primary and
  ///      interpolated spokes to the target boundary
  /// L1 - measures the overall deviation of the spokes direction from perpendicularity
  ///      to the boundary
  /// L2 - meausures the degree to violating local self overlap condition
  /// 
  /// For more info see
  /// Liu, Z., Hong, J., Vicory, J., Damon, J. N., & Pizer, S. M. (2021).
  /// Fitting unbranching skeletal structures to objects.
  /// Medical Image Analysis, 70, 102020.
  double EvaluateObjectiveFunction(double* coeff, SpokeType spokeType) {
    // this function cannot throw because it will cause a memory leak in min_newuoa
    try {
      auto tempSRep = this->Refine(*m_srep, coeff, spokeType);
      auto interpolatedTempSRep = m_srepLogic->SmartInterpolateSRep(*tempSRep, m_interpolationLevel);

      const auto L0AndL1 = ComputeDistanceSquaredAndNormalToImage(*interpolatedTempSRep, spokeType);
      const auto& distanceSquared = L0AndL1.first; // L0
      const auto& normalPenalty = L0AndL1.second; // L1

      const auto srad = ComputeRSradPenalty(*interpolatedTempSRep, spokeType); // L2

      const auto val =  distanceSquared * m_L0Weight + normalPenalty * m_L1Weight + srad * m_L2Weight;
      this->IncrementIteration();
      std::cout  << "Eval func " << m_iteration << ": " << val <<
        " = " << (distanceSquared * m_L0Weight) << " + " << (normalPenalty * m_L1Weight) << " + " << (srad * m_L2Weight) << std::endl;
      return val;
    } catch (const std::exception& e) {
      std::cerr << "Error in SRepRefinement evaluating objective function: " << e.what() << std::endl;
      return 1e10;
    } catch (...) {
      std::cerr << "Unknown error in SRepRefinement evaluating objective function" << std::endl;
      return 1e10;
    }
  }

  //---------------------------------------------------------------------------
  void GetInitialCoefficients() {
    const auto numLines = m_srep->GetNumberOfLines();
    const auto numSteps = m_srep->GetNumberOfSteps();

    m_flattenedUpCoeff.reserve(numLines * numSteps * 4);
    m_flattenedDownCoeff.reserve(numLines * numSteps * 4);
    for (IndexType l = 0; l < numLines; ++l) {
      for (IndexType s = 0; s < numSteps; ++s) {
        const auto upUnitDir = m_srep->GetSkeletalPoint(l, s)->GetUpSpoke()->GetDirection().Unit();
        m_flattenedUpCoeff.push_back(upUnitDir[0]);
        m_flattenedUpCoeff.push_back(upUnitDir[1]);
        m_flattenedUpCoeff.push_back(upUnitDir[2]);
        m_flattenedUpCoeff.push_back(0); // initial radius starts at 0

        const auto downUnitDir = m_srep->GetSkeletalPoint(l, s)->GetDownSpoke()->GetDirection().Unit();
        m_flattenedDownCoeff.push_back(downUnitDir[0]);
        m_flattenedDownCoeff.push_back(downUnitDir[1]);
        m_flattenedDownCoeff.push_back(downUnitDir[2]);
        m_flattenedDownCoeff.push_back(0); // initial radius starts at 0
      }
    }
  }
}; // class Refiner

//---------------------------------------------------------------------------
vtkSmartPointer<vtkEllipticalSRep> RefineSRep(
  const vtkEllipticalSRep& srep,
  vtkPolyData* polyData,
  double initialRegionSize,
  double finalRegionSize,
  int maxIterations,
  int interpolationLevel,
  double L0Weight,
  double L1Weight,
  double L2Weight,
  ProgressCallbackFunction progressCallback)
{
  Refiner refiner(srep, polyData, initialRegionSize, finalRegionSize, maxIterations, interpolationLevel, L0Weight, L1Weight, L2Weight);
  refiner.SetProgressCallback(progressCallback);
  return refiner.Run();
}

} //namespace {}

//----------------------------------------------------------------------------
vtkStandardNewMacro(vtkSlicerSRepRefinementLogic);

//----------------------------------------------------------------------------
vtkSlicerSRepRefinementLogic::vtkSlicerSRepRefinementLogic() = default;

//----------------------------------------------------------------------------
vtkSlicerSRepRefinementLogic::~vtkSlicerSRepRefinementLogic() = default;

//----------------------------------------------------------------------------
void vtkSlicerSRepRefinementLogic::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}

//---------------------------------------------------------------------------
void vtkSlicerSRepRefinementLogic::ProgressCallback(double progress) {
  this->InvokeEvent(vtkCommand::ProgressEvent, &progress);
}

//---------------------------------------------------------------------------
vtkMRMLEllipticalSRepNode* vtkSlicerSRepRefinementLogic::Run(
  vtkMRMLModelNode* model,
  vtkMRMLEllipticalSRepNode* srepNode,
  double initialRegionSize,
  double finalRegionSize,
  int maxIterations,
  int interpolationLevel,
  double L0Weight,
  double L1Weight,
  double L2Weight)
{
  vtkSmartPointer<vtkMRMLScene> scene = this->GetMRMLScene();
  if (!scene) {
    throw std::runtime_error("Can't add new vtkMRMLEllipticalSRepNode with null scene");
  }
  vtkMRMLEllipticalSRepNode* destination = vtkMRMLEllipticalSRepNode::SafeDownCast(scene->AddNewNodeByClass("vtkMRMLEllipticalSRepNode"));
  try {
    this->Run(model, srepNode, initialRegionSize, finalRegionSize, maxIterations, interpolationLevel, L0Weight, L1Weight, L2Weight, destination);
    return destination;
  } catch (...) {
    scene->RemoveNode(destination);
    throw;
  }
}

//---------------------------------------------------------------------------
void vtkSlicerSRepRefinementLogic::Run(
  vtkMRMLModelNode* model,
  vtkMRMLEllipticalSRepNode* srepNode,
  double initialRegionSize,
  double finalRegionSize,
  int maxIterations,
  int interpolationLevel,
  double L0Weight,
  double L1Weight,
  double L2Weight,
  vtkMRMLEllipticalSRepNode* destination)
{
  try {
    if (!model) {
      throw std::invalid_argument("Cannot refine an SRep with a null model");
    }
    if (!srepNode || !srepNode->GetSRep() || srepNode->GetSRep()->IsEmpty()) {
      throw std::invalid_argument("Cannot refine an SRep with a null srep");
    }
    if (maxIterations < 1) {
      throw std::invalid_argument("must have at least one iteration");
    }
    if (interpolationLevel < 0) {
      throw std::invalid_argument("interpolation level must be non-negative");
    }

    auto refinedSRep = RefineSRep(
      *srepNode->GetEllipticalSRep(),
      model->GetPolyData(),
      initialRegionSize,
      finalRegionSize,
      maxIterations,
      interpolationLevel,
      L0Weight,
      L1Weight,
      L2Weight,
      [this](double p){ this->ProgressCallback(p); });
    destination->SetEllipticalSRep(refinedSRep);
  } catch (const std::exception& e) {
    vtkErrorMacro("Error running SRep refinement: " << e.what());
    throw;
  }
  catch (...) {
    vtkErrorMacro("Unknown error running SRep refinement");
    throw;
  }
}
