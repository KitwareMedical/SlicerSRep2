#include <srep/Point3d.h>

#include <cmath>
#include <stdexcept>

#include <vtkMath.h>

namespace srep {

Point3d::Point3d()
    : X(0.0)
    , Y(0.0)
    , Z(0.0)
{}

Point3d::Point3d(double x, double y, double z)
    : X(x)
    , Y(y)
    , Z(z)
{
    if (std::isnan(x) || std::isnan(y) || std::isnan(z)) {
        throw std::invalid_argument("Point cannot have a nan component");
    }
}

Point3d::Point3d(const double p[3])
    : Point3d(p[0], p[1], p[2])
{}

void Point3d::SetX(double x) {
    if (std::isnan(x)) {
        throw std::invalid_argument("Point cannot have a nan component");
    }
    this->X = x;
}

void Point3d::SetY(double y) {
    if (std::isnan(y)) {
        throw std::invalid_argument("Point cannot have a nan component");
    }
    this->Y = y;
}

void Point3d::SetZ(double z) {
    if (std::isnan(z)) {
        throw std::invalid_argument("Point cannot have a nan component");
    }
    this->Z = z;
}

std::array<double, 3> Point3d::AsArray() const {
    return std::array<double, 3>{this->GetX(), this->GetY(), this->GetZ()};
}

double Point3d::Distance(const Point3d& a, const Point3d& b) {
    const auto distSquared = vtkMath::Distance2BetweenPoints(a.AsArray().data(), b.AsArray().data());
    return pow(distSquared, 0.5);
}

bool operator< (const Point3d& a, const Point3d& b) {
    return (a.GetX() != b.GetX()) ? (a.GetX() < b.GetX())
        : ((a.GetY() != b.GetY()) ? (a.GetY() < b.GetY()) : (a.GetZ() < b.GetZ()));
}
bool operator==(const Point3d& a, const Point3d& b) {
    return !(a < b) && !(b < a);
}
bool operator!=(const Point3d& a, const Point3d& b) {
    return (a < b) || (b < a);
}
bool operator> (const Point3d& a, const Point3d& b) {
    return b < a;
}
bool operator<=(const Point3d& a, const Point3d& b) {
    return !(b < a);
}
bool operator>=(const Point3d& a, const Point3d& b) {
    return !(a < b);
}

std::ostream& operator<<(std::ostream& os, const Point3d& point) {
    os << "(" << point.GetX() << ", " << point.GetY() << ", " << point.GetZ() << ")";
    return os;
}

}
