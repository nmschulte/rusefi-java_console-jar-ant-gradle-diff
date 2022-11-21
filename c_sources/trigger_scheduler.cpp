#include "pch.h"

#include "event_queue.h"

bool TriggerScheduler::assertNotInList(AngleBasedEventBase *head, AngleBasedEventBase *element) {
       assertNotInListMethodBody(AngleBasedEventBase, head, element, nextToothEvent)
}

/**
 * Schedules 'action' to occur at engine cycle angle 'angle'.
 *
 * If you know when a recent trigger occured, you can pass it in as 'trgEventIndex' and
 * 'edgeTimestamp'.  Otherwise pass in TRIGGER_EVENT_UNDEFINED and the work will be scheduled on
 * the next trigger edge.
 *
 * @return true if event corresponds to current tooth and was time-based scheduler
 *         false if event was put into queue for scheduling at a later tooth
 */
bool TriggerScheduler::scheduleOrQueue(AngleBasedEventOld *event,
		uint32_t trgEventIndex,
		efitick_t edgeTimestamp,
		angle_t angle,
		action_s action) {
	event->position.setAngle(angle);

	/**
	 * Here's the status as of Jan 2020:
	 * Once we hit the last trigger tooth prior to needed event, schedule it by time.  We use
	 * as much trigger position angle as possible and only use less precise RPM-based time
	 * calculation for the last portion of the angle, the one between two teeth closest to the
	 * desired angle moment.
	 */
	if (trgEventIndex != TRIGGER_EVENT_UNDEFINED && event->position.triggerEventIndex == trgEventIndex) {
		/**
		 * Spark should be fired before the next trigger event - time-based delay is best precision possible
		 */
		scheduling_s * sDown = &event->scheduling;

		scheduleByAngle(
			sDown,
			edgeTimestamp,
			event->position.angleOffsetFromTriggerEvent,
			action
		);

		return true;
	} else {
		event->action = action;
		/**
		 * Spark should be scheduled in relation to some future trigger event, this way we get better firing precision
		 */
		{
			chibios_rt::CriticalSectionLocker csl;

			// TODO: This is O(n), consider some other way of detecting if in a list,
			// and consider doubly linked or other list tricks.

			if (!assertNotInList(m_angleBasedEventsHead, event)) {
				// Use Append to retain some semblance of event ordering in case of
				// time skew.  Thus on events are always followed by off events.
				LL_APPEND2(m_angleBasedEventsHead, event, nextToothEvent);

				return false;
			}
		}
		engine->outputChannels.systemEventReuse++; // not atomic/not volatile but good enough for just debugging
#if SPARK_EXTREME_LOGGING
		efiPrintf("isPending thus not adding to queue index=%d rev=%d now=%d",
			  trgEventIndex, getRevolutionCounter(), (int)getTimeNowUs());
#endif /* SPARK_EXTREME_LOGGING */
		return false;
	}
}

bool TriggerScheduler::scheduleOrQueue(AngleBasedEventNew *event,
		uint32_t trgEventIndex,
		efitick_t edgeTimestamp,
		angle_t angle,
		action_s action,
		float currentPhase, float nextPhase) {
	event->enginePhase = angle;

	if (event->shouldSchedule(trgEventIndex, currentPhase, nextPhase)) {
		// if we're due now, just schedule the event
		scheduleByAngle(
			&event->scheduling,
			edgeTimestamp,
			event->getAngleFromNow(currentPhase),
			action
		);

		return true;
	} else {
		// Not due at this tooth, add to the list to execute later
		event->action = action;

		{
			chibios_rt::CriticalSectionLocker csl;

			// TODO: This is O(n), consider some other way of detecting if in a list,
			// and consider doubly linked or other list tricks.

			if (!assertNotInList(m_angleBasedEventsHead, event)) {
				// Use Append to retain some semblance of event ordering in case of
				// time skew.  Thus on events are always followed by off events.
				LL_APPEND2(m_angleBasedEventsHead, event, nextToothEvent);

				return false;
			}
		}
	}

	return false;
}

void TriggerScheduler::scheduleEventsUntilNextTriggerTooth(int rpm,
							   uint32_t trgEventIndex,
							   efitick_t edgeTimestamp, float currentPhase, float nextPhase) {

	if (!isValidRpm(rpm)) {
		 // this might happen for instance in case of a single trigger event after a pause
		return;
	}

	AngleBasedEventBase *current, *tmp, *keephead;
	AngleBasedEventBase *keeptail = nullptr;

	{
		chibios_rt::CriticalSectionLocker csl;

		keephead = m_angleBasedEventsHead;
		m_angleBasedEventsHead = nullptr;
	}

	LL_FOREACH_SAFE2(keephead, current, tmp, nextToothEvent)
	{
		if (current->shouldSchedule(trgEventIndex, currentPhase, nextPhase)) {
			// time to fire a spark which was scheduled previously

			// Yes this looks like O(n^2), but that's only over the entire engine
			// cycle.  It's really O(mn + nn) where m = # of teeth and n = # events
			// fired per cycle.  The number of teeth outweigh the number of events, at
			// least for 60-2....  So odds are we're only firing an event or two per
			// tooth, which means the outer loop is really only O(n).  And if we are
			// firing many events per teeth, then it's likely the events before this
			// one also fired and thus the call to LL_DELETE2 is closer to O(1).
			LL_DELETE2(keephead, current, nextToothEvent);

			scheduling_s * sDown = &current->scheduling;

#if SPARK_EXTREME_LOGGING
			efiPrintf("time to invoke ind=%d %d %d",
				  trgEventIndex, getRevolutionCounter(), (int)getTimeNowUs());
#endif /* SPARK_EXTREME_LOGGING */

			// In case this event was scheduled by overdwell protection, cancel it so
			// we can re-schedule at the correct time
			engine->executor.cancel(sDown);

			scheduleByAngle(
				sDown,
				edgeTimestamp,
				current->getAngleFromNow(currentPhase),
				current->action
			);
		} else {
			keeptail = current; // Used for fast list concatenation
		}
	}

	if (keephead) {
		chibios_rt::CriticalSectionLocker csl;

		// Put any new entries onto the end of the keep list
		keeptail->nextToothEvent = m_angleBasedEventsHead;
		m_angleBasedEventsHead = keephead;
	}
}

bool AngleBasedEventOld::shouldSchedule(uint32_t trgEventIndex, float /*currentPhase*/, float /*nextPhase*/) const {
	return position.triggerEventIndex == trgEventIndex;
}

float AngleBasedEventOld::getAngleFromNow(float /*currentPhase*/) const {
	return position.angleOffsetFromTriggerEvent;
}

bool AngleBasedEventNew::shouldSchedule(uint32_t /*trgEventIndex*/, float currentPhase, float nextPhase) const {
	return isPhaseInRange(this->enginePhase, currentPhase, nextPhase);
}

float AngleBasedEventNew::getAngleFromNow(float currentPhase) const {
	float angleOffset = this->enginePhase - currentPhase;
	if (angleOffset < 0) {
		angleOffset += engine->engineState.engineCycle;
	}

	return angleOffset;
}

#if EFI_UNIT_TEST
// todo: reduce code duplication with another 'getElementAtIndexForUnitText'
AngleBasedEventBase * TriggerScheduler::getElementAtIndexForUnitTest(int index) {
	AngleBasedEventBase * current;

	LL_FOREACH2(m_angleBasedEventsHead, current, nextToothEvent)
	{
		if (index == 0)
			return current;
		index--;
	}
	firmwareError(OBD_PCM_Processor_Fault, "getElementAtIndexForUnitText: null");
	return nullptr;
}
#endif /* EFI_UNIT_TEST */
