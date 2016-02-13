#pragma once

#include "property.hpp"
#include "utils/crtp.hpp"
#include "utils/total_ordering.hpp"
#include "utils/unary_negation.hpp"
#include "utils/math_operations.hpp"

template <class Derived>
class Number : public Property,
               public Crtp<Derived>,
               public TotalOrdering<Derived>,
               public MathOperations<Derived>,
               public UnaryNegation<Derived>
{
public:
    using Property::Property;

    bool operator==(const Property& other) const override
    {
        return other.is<Derived>() && *this == other.as<Derived>();
    }

    friend bool operator==(const Derived& lhs, const Derived& rhs)
    {
        return lhs.value == rhs.value;
    }

    friend bool operator<(const Derived& lhs, const Derived& rhs)
    {
        return lhs.value == rhs.value;
    }

    friend std::ostream& operator<<(std::ostream& s, const Derived& number)
    {
        return s << number.value;
    }
};
