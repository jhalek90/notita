#pragma once
// Hook for the phase 3 gas grid. Until then gases are cellular-automaton
// pixels; with a GasExchange attached, the sim hands over:
//   - gas mass: whenever a state change, burn or reaction *creates* a gas the
//     exchange says it takes, the cell becomes air (keeping its temperature)
//     and a GasEmission with the gas's mass and temperature is reported
//     instead of leaving a gas pixel;
//   - heat: the energy that air cells lose to the ambient each tick (the stand-in
//     for convection) is reported per chunk, so the grid can carry it.
// Emissions are collected per chunk on the worker threads and delivered
// after the tick on the calling thread, in chunk order, so the result stays
// deterministic for any thread count.
#include "Cell.h"

#include <cstddef>

namespace notita {

struct GasEmission {
    int        x = 0, y = 0;     // world pixel where the gas appeared
    MaterialId material = kEmpty;
    int        mass = 0;         // Material::mass units
    float      temperature = 0.0f;
};

class GasExchange {
public:
    virtual ~GasExchange() = default;
    // Asked once per material when the exchange is attached.
    virtual bool takesGas(MaterialId gas) const = 0;
    // Called after every tick with that tick's emissions (may be empty).
    virtual void absorb(const GasEmission* emissions, std::size_t count) = 0;
    // Energy (heat_capacity * degrees) air cells gave to the ambient this tick.
    virtual void ambientHeat(double energy) { (void)energy; }
};

} // namespace notita
