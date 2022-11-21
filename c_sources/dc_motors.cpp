/**
 * @file dc_motors.cpp
 *
 * @date March 3, 2020
 * @author Matthew Kennedy (c) 2020
 */

#include "pch.h"

#include "periodic_task.h"

#include "dc_motors.h"
#include "dc_motor.h"

// Simple wrapper to use an OutputPin as "PWM" that can only do 0 or 1
struct PwmWrapper : public IPwm {
	OutputPin& m_pin;

	PwmWrapper(OutputPin& pin) : m_pin(pin) { }

	void setSimplePwmDutyCycle(float dutyCycle) override {
		m_pin.setValue(dutyCycle > 0.5f);
	}
};

class DcHardware {
private:
	OutputPin m_pinEnable;
	OutputPin m_pinDir1;
	OutputPin m_pinDir2;
	OutputPin m_disablePin;

	PwmWrapper wrappedEnable{m_pinEnable};
	PwmWrapper wrappedDir1{m_pinDir1};
	PwmWrapper wrappedDir2{m_pinDir2};

	SimplePwm m_pwm1;
	SimplePwm m_pwm2;

	bool isStarted = false;

public:
	DcHardware() : dcMotor(m_disablePin) {}

	TwoPinDcMotor dcMotor;
	
	void setFrequency(int frequency) {
		m_pwm1.setFrequency(frequency);
		m_pwm2.setFrequency(frequency);
	}

	void stop() {
		// todo: replace 'isStarted' with 'stop'
	}

	void start(bool useTwoWires, 
			brain_pin_e pinEnable,
			brain_pin_e pinDir1,
			brain_pin_e pinDir2,
			brain_pin_e pinDisable,
			bool isInverted,
			ExecutorInterface* executor,
			int frequency) {

		if (isStarted) {
			// actually implement stop()
			return;
		}
		isStarted = true;

		dcMotor.setType(useTwoWires ? TwoPinDcMotor::ControlType::PwmDirectionPins : TwoPinDcMotor::ControlType::PwmEnablePin);

		// Configure the disable pin first - ensure things are in a safe state
		m_disablePin.initPin("ETB Disable", pinDisable);
		m_disablePin.setValue(0);

		// Clamp to >100hz
		int clampedFrequency = maxI(100, frequency);

		if (clampedFrequency > ETB_HW_MAX_FREQUENCY) {
			firmwareError(OBD_PCM_Processor_Fault, "Electronic throttle frequency too high, maximum %d hz", ETB_HW_MAX_FREQUENCY);
			return;
		}

		if (useTwoWires) {
			m_pinEnable.initPin("ETB Enable", pinEnable);

// no need to complicate event queue with ETB PWM in unit tests
#if ! EFI_UNIT_TEST
			startSimplePwmHard(&m_pwm1, "ETB Dir 1",
				executor,
				pinDir1,
				&m_pinDir1,
				clampedFrequency,
				0
			);

			startSimplePwmHard(&m_pwm2, "ETB Dir 2",
				executor,
				pinDir2,
				&m_pinDir2,
				clampedFrequency,
				0
			);
#endif // EFI_UNIT_TEST

			dcMotor.configure(wrappedEnable, m_pwm1, m_pwm2, isInverted);
		} else {
			m_pinDir1.initPin("ETB Dir 1", pinDir1);
			m_pinDir2.initPin("ETB Dir 2", pinDir2);

// no need to complicate event queue with ETB PWM in unit tests
#if ! EFI_UNIT_TEST
			startSimplePwmHard(&m_pwm1, "ETB Enable",
				executor,
				pinEnable,
				&m_pinEnable,
				clampedFrequency,
				0
			);
#endif // EFI_UNIT_TEST

			dcMotor.configure(m_pwm1, wrappedDir1, wrappedDir2, isInverted);
		}
	}
};

static DcHardware dcHardware[ETB_COUNT + DC_PER_STEPPER];

DcMotor* initDcMotor(const dc_io& io, size_t index, bool useTwoWires) {
	auto& hw = dcHardware[index];

	hw.start(
		useTwoWires,
		io.controlPin,
		io.directionPin1,
		io.directionPin2,
		io.disablePin,
		// todo You would not believe how you invert TLE9201 #4579
		engineConfiguration->stepperDcInvertedPins,
		&engine->executor,
		engineConfiguration->etbFreq
	);

	return &hw.dcMotor;
}

DcMotor* initDcMotor(brain_pin_e coil_p, brain_pin_e coil_m, size_t index) {
	auto& hw = dcHardware[index];

	hw.start(
		true, /* useTwoWires */
		Gpio::Unassigned, /* pinEnable */
		coil_p,
		coil_m,
		Gpio::Unassigned, /* pinDisable */
		engineConfiguration->stepperDcInvertedPins,
		&engine->executor,
		engineConfiguration->etbFreq /* same in case of stepper? */
	);

	return &hw.dcMotor;
}

void setDcMotorFrequency(size_t index, int hz) {
	dcHardware[index].setFrequency(hz);
}

void setDcMotorDuty(size_t index, float duty) {
	dcHardware[index].dcMotor.set(duty);
}


void showDcMotorInfo(int i) {
	DcHardware *dc = &dcHardware[i];

	efiPrintf(" motor: dir=%d DC=%f", dc->dcMotor.isOpenDirection(), dc->dcMotor.get());
}

