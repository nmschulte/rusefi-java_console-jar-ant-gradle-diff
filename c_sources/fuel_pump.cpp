/**
 * low pressure fuel pump control
 * for high-pressure see HpfpController@high_pressure_fuel_pump
 *
*/
#include "pch.h"

#include "fuel_pump.h"

void FuelPumpController::onSlowCallback() {
	auto timeSinceIgn = m_ignOnTimer.getElapsedSeconds();

	// If the ignition just turned on, turn on the fuel pump to prime
	isPrime = timeSinceIgn >= 0 && timeSinceIgn < engineConfiguration->startUpFuelPumpDuration;

	// If there was a trigger event recently, turn on the pump, the engine is running!
	engineTurnedRecently = engine->triggerCentral.engineMovedRecently();

	isFuelPumpOn = isPrime || engineTurnedRecently;

	enginePins.fuelPumpRelay.setValue(isFuelPumpOn);
}

void FuelPumpController::onIgnitionStateChanged(bool ignitionOnParam) {
	// live data parser convention is asking for a field
	ignitionOn = ignitionOnParam;
	if (ignitionOn) {
		m_ignOnTimer.reset();
	}
}
