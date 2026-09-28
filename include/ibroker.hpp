#pragma once
#include "aggregator.hpp" // Oder wo auch immer TradeSignal definiert ist

class IBroker {
public:
    virtual ~IBroker() = default;
    
    // Die Pflicht-Methoden für jeden Broker (Paper oder Live)
    virtual double GetBalance() const = 0;
    virtual bool HasOpenPosition() const = 0;
    virtual TradeSignal GetCurrentPosition() const = 0;
    virtual void UpdateStopLoss(double new_sl) = 0;
};