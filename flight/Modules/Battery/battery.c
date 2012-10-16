/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup BatteryModule Battery Module
 * @brief Measures battery voltage and current
 * Updates the FlightBatteryState object
 * @{
 *
 * @file       battery.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to read the battery Voltage and Current periodically and set alarms appropriately.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Output object: FlightBatteryState
 *
 * This module will periodically generate information on the battery state.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "openpilot.h"

#include "flightbatterystate.h"
#include "flightbatterysettings.h"

//
// Configuration
//
#define SAMPLE_PERIOD_MS		500

//#define ENABLE_DEBUG_MSG

#ifdef ENABLE_DEBUG_MSG
#define DEBUG_PORT			PIOS_COM_GPS
#define DEBUG_MSG(format, ...) PIOS_COM_SendFormattedString(DEBUG_PORT, format, ## __VA_ARGS__)
#else
#define DEBUG_MSG(format, ...)
#endif

// Private types

// Private variables

// Private functions
static void onTimer(UAVObjEvent* ev);

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
MODULE_INITCALL(BatteryInitialize, 0)

int32_t BatteryInitialize(void)
{
	BatteryStateInitialze();
	BatterySettingsInitialize();
	
	static UAVObjEvent ev;

	memset(&ev,0,sizeof(UAVObjEvent));
	EventPeriodicCallbackCreate(&ev, onTimer, SAMPLE_PERIOD_MS / portTICK_RATE_MS);

	return 0;
}

static void onTimer(UAVObjEvent* ev)
{
	static portTickType lastSysTime;
	static bool firstRun = true;

	static FlightBatteryStateData flightBatteryData;

	if (firstRun) {
		#ifdef ENABLE_DEBUG_MSG
			PIOS_COM_ChangeBaud(DEBUG_PORT, 57600);
		#endif
		lastSysTime = xTaskGetTickCount();
		//FlightBatteryStateGet(&flightBatteryData);

		firstRun = false;
	}


	AlarmsSet(SYSTEMALARMS_ALARM_BATTERY, SYSTEMALARMS_ALARM_ERROR);


	portTickType thisSysTime;
	FlightBatterySettingsData batterySettings;
	static float dT = SAMPLE_PERIOD_MS / 1000;
	float Bob;
	float energyRemaining;


	// Check how long since last update
	thisSysTime = xTaskGetTickCount();
	if(thisSysTime > lastSysTime) // reuse dt in case of wraparound
		dT = (float)(thisSysTime - lastSysTime) / (float)(portTICK_RATE_MS * 1000.0f);
	//lastSysTime = thisSysTime;

	FlightBatterySettingsGet(&batterySettings);

	//calculate the battery parameters
	flightBatteryData.Voltage = ((float)PIOS_ADC_PinGet(2)) * batterySettings.SensorCalibrations[FLIGHTBATTERYSETTINGS_SENSORCALIBRATIONS_VOLTAGEFACTOR]; //in Volts
	flightBatteryData.Current = ((float)PIOS_ADC_PinGet(1)) * batterySettings.SensorCalibrations[FLIGHTBATTERYSETTINGS_SENSORCALIBRATIONS_CURRENTFACTOR]; //in Amps
Bob =dT; // FIXME: something funky happens if I don't do this... Andrew
	flightBatteryData.ConsumedEnergy += (flightBatteryData.Current * 1000.0 * dT / 3600.0) ;//in mAh

	if (flightBatteryData.Current > flightBatteryData.PeakCurrent)flightBatteryData.PeakCurrent = flightBatteryData.Current; //in Amps
	flightBatteryData.AvgCurrent=(flightBatteryData.AvgCurrent*0.8)+(flightBatteryData.Current*0.2); //in Amps

	//sanity checks
	if (flightBatteryData.AvgCurrent<0)flightBatteryData.AvgCurrent=0.0;
	if (flightBatteryData.PeakCurrent<0)flightBatteryData.PeakCurrent=0.0;
	if (flightBatteryData.ConsumedEnergy<0)flightBatteryData.ConsumedEnergy=0.0;

	energyRemaining = batterySettings.Capacity - flightBatteryData.ConsumedEnergy; // in mAh
	flightBatteryData.EstimatedFlightTime = ((energyRemaining / (flightBatteryData.AvgCurrent*1000.0))*3600.0);//in Sec

	//generate alarms where needed...
	if ((flightBatteryData.Voltage<=0)&&(flightBatteryData.Current<=0))
	{
		AlarmsSet(SYSTEMALARMS_ALARM_BATTERY, SYSTEMALARMS_ALARM_ERROR);
		AlarmsSet(SYSTEMALARMS_ALARM_FLIGHTTIME, SYSTEMALARMS_ALARM_ERROR);
	}
	else
	{
		if (flightBatteryData.EstimatedFlightTime < 30) AlarmsSet(SYSTEMALARMS_ALARM_FLIGHTTIME, SYSTEMALARMS_ALARM_CRITICAL);
		else if (flightBatteryData.EstimatedFlightTime < 60) AlarmsSet(SYSTEMALARMS_ALARM_FLIGHTTIME, SYSTEMALARMS_ALARM_WARNING);
		else AlarmsClear(SYSTEMALARMS_ALARM_FLIGHTTIME);

		// FIXME: should make the battery voltage detection dependent on battery type.
		if (flightBatteryData.Voltage < batterySettings.VoltageThresholds[FLIGHTBATTERYSETTINGS_VOLTAGETHRESHOLDS_ALARM])
			AlarmsSet(SYSTEMALARMS_ALARM_BATTERY, SYSTEMALARMS_ALARM_CRITICAL);
		else if (flightBatteryData.Voltage < batterySettings.VoltageThresholds[FLIGHTBATTERYSETTINGS_VOLTAGETHRESHOLDS_WARNING])
			AlarmsSet(SYSTEMALARMS_ALARM_BATTERY, SYSTEMALARMS_ALARM_WARNING);
		else AlarmsClear(SYSTEMALARMS_ALARM_BATTERY);
	}
	lastSysTime = thisSysTime;

	FlightBatteryStateSet(&flightBatteryData);
}

/**
  * @}
  */

/**
 * @}
 */