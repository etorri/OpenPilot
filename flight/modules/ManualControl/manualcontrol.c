/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup ManualControlModule Manual Control Module
 * @brief Provide manual control or allow it alter flight mode.
 * @{
 *
 * Reads in the ManualControlCommand FlightMode setting from receiver then either
 * pass the settings straght to ActuatorDesired object (manual mode) or to
 * AttitudeDesired object (stabilized mode)
 *
 * @file       manualcontrol.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      ManualControl module. Handles safety R/C link and flight mode.
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

#include <openpilot.h>
#include <pios_struct_helper.h>
#include "accessorydesired.h"
#include "actuatordesired.h"
#include "altitudeholddesired.h"
#include "flighttelemetrystats.h"
#include "flightstatus.h"
#include "sanitycheck.h"
#include "manualcontrol.h"
#include "manualcontrolsettings.h"
#include "manualcontrolcommand.h"
#include "positionstate.h"
#include "pathdesired.h"
#include "stabilizationbank.h"
#include "stabilizationdesired.h"
#include "receiveractivity.h"
#include "systemsettings.h"
#include <altitudeholdsettings.h>
#include <taskinfo.h>

#if defined(PIOS_INCLUDE_USB_RCTX)
#include "pios_usb_rctx.h"
#endif /* PIOS_INCLUDE_USB_RCTX */

// Private constants
#if defined(PIOS_MANUAL_STACK_SIZE)
#define STACK_SIZE_BYTES  PIOS_MANUAL_STACK_SIZE
#else
#define STACK_SIZE_BYTES  1152
#endif

#define TASK_PRIORITY     (tskIDLE_PRIORITY + 3) // 3 = flight control
#define UPDATE_PERIOD_MS  20
#define THROTTLE_FAILSAFE -0.1f
#define ARMED_THRESHOLD   0.50f
// safe band to allow a bit of calibration error or trim offset (in microseconds)
#define CONNECTION_OFFSET 250

// Private types
typedef enum {
    ARM_STATE_DISARMED,
    ARM_STATE_ARMING_MANUAL,
    ARM_STATE_ARMED,
    ARM_STATE_DISARMING_MANUAL,
    ARM_STATE_DISARMING_TIMEOUT
} ArmState_t;

// Private variables
static xTaskHandle taskHandle;
static ArmState_t armState;
static portTickType lastSysTime;

#ifdef USE_INPUT_LPF
static portTickType lastSysTimeLPF;
static float inputFiltered[MANUALCONTROLSETTINGS_RESPONSETIME_NUMELEM];
#endif

// Private functions
static void updateActuatorDesired(ManualControlCommandData *cmd);
static void updateStabilizationDesired(ManualControlCommandData *cmd, ManualControlSettingsData *settings);
static void updateLandDesired(ManualControlCommandData *cmd, bool changed);
static void altitudeHoldDesired(ManualControlCommandData *cmd, bool changed);
static void updatePathDesired(ManualControlCommandData *cmd, bool changed, bool home);
static void processFlightMode(ManualControlSettingsData *settings, float flightMode, ManualControlCommandData *cmd);
static void processArm(ManualControlCommandData *cmd, ManualControlSettingsData *settings, int8_t armSwitch);
static void setArmedIfChanged(uint8_t val);
static void configurationUpdatedCb(UAVObjEvent *ev);

static void manualControlTask(void *parameters);
static float scaleChannel(int16_t value, int16_t max, int16_t min, int16_t neutral);
static uint32_t timeDifferenceMs(portTickType start_time, portTickType end_time);
static bool okToArm(void);
static bool validInputRange(int16_t min, int16_t max, uint16_t value);
static void applyDeadband(float *value, float deadband);

#ifdef USE_INPUT_LPF
static void applyLPF(float *value, ManualControlSettingsResponseTimeElem channel, ManualControlSettingsData *settings, float dT);
#endif

#define RCVR_ACTIVITY_MONITOR_CHANNELS_PER_GROUP 12
#define RCVR_ACTIVITY_MONITOR_MIN_RANGE          10
struct rcvr_activity_fsm {
    ManualControlSettingsChannelGroupsOptions group;
    uint16_t prev[RCVR_ACTIVITY_MONITOR_CHANNELS_PER_GROUP];
    uint8_t sample_count;
};
static struct rcvr_activity_fsm activity_fsm;

static void resetRcvrActivity(struct rcvr_activity_fsm *fsm);
static bool updateRcvrActivity(struct rcvr_activity_fsm *fsm);

#define assumptions (assumptions1 && assumptions3 && assumptions5 && assumptions_flightmode && assumptions_channelcount)

/**
 * Module starting
 */
int32_t ManualControlStart()
{
    // Start main task
    xTaskCreate(manualControlTask, (signed char *)"ManualControl", STACK_SIZE_BYTES / 4, NULL, TASK_PRIORITY, &taskHandle);
    PIOS_TASK_MONITOR_RegisterTask(TASKINFO_RUNNING_MANUALCONTROL, taskHandle);
#ifdef PIOS_INCLUDE_WDG
    PIOS_WDG_RegisterFlag(PIOS_WDG_MANUAL);
#endif

    return 0;
}

/**
 * Module initialization
 */
int32_t ManualControlInitialize()
{
    /* Check the assumptions about uavobject enum's are correct */
    if (!assumptions) {
        return -1;
    }

    AccessoryDesiredInitialize();
    ManualControlCommandInitialize();
    FlightStatusInitialize();
    StabilizationDesiredInitialize();
    ReceiverActivityInitialize();
    ManualControlSettingsInitialize();

    return 0;
}
MODULE_INITCALL(ManualControlInitialize, ManualControlStart);

/**
 * Module task
 */
static void manualControlTask(__attribute__((unused)) void *parameters)
{
    ManualControlSettingsData settings;
    ManualControlCommandData cmd;
    FlightStatusData flightStatus;
    float flightMode = 0;

    uint8_t disconnected_count = 0;
    uint8_t connected_count    = 0;

    // For now manual instantiate extra instances of Accessory Desired.  In future should be done dynamically
    // this includes not even registering it if not used
    AccessoryDesiredCreateInstance();
    AccessoryDesiredCreateInstance();

    // Run this initially to make sure the configuration is checked
    configuration_check();

    // Whenever the configuration changes, make sure it is safe to fly
    SystemSettingsConnectCallback(configurationUpdatedCb);
    ManualControlSettingsConnectCallback(configurationUpdatedCb);

    // Whenever the configuration changes, make sure it is safe to fly

    // Make sure unarmed on power up
    ManualControlCommandGet(&cmd);
    FlightStatusGet(&flightStatus);
    flightStatus.Armed = FLIGHTSTATUS_ARMED_DISARMED;
    armState = ARM_STATE_DISARMED;

    /* Initialize the RcvrActivty FSM */
    portTickType lastActivityTime = xTaskGetTickCount();
    resetRcvrActivity(&activity_fsm);

    // Main task loop
    lastSysTime = xTaskGetTickCount();

    float scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_NUMELEM] = { 0 };

    while (1) {
        // Wait until next update
        vTaskDelayUntil(&lastSysTime, UPDATE_PERIOD_MS / portTICK_RATE_MS);
#ifdef PIOS_INCLUDE_WDG
        PIOS_WDG_UpdateFlag(PIOS_WDG_MANUAL);
#endif

        // Read settings
        ManualControlSettingsGet(&settings);

        /* Update channel activity monitor */
        if (flightStatus.Armed == ARM_STATE_DISARMED) {
            if (updateRcvrActivity(&activity_fsm)) {
                /* Reset the aging timer because activity was detected */
                lastActivityTime = lastSysTime;
            }
        }
        if (timeDifferenceMs(lastActivityTime, lastSysTime) > 5000) {
            resetRcvrActivity(&activity_fsm);
            lastActivityTime = lastSysTime;
        }

        if (ManualControlCommandReadOnly()) {
            FlightTelemetryStatsData flightTelemStats;
            FlightTelemetryStatsGet(&flightTelemStats);
            if (flightTelemStats.Status != FLIGHTTELEMETRYSTATS_STATUS_CONNECTED) {
                /* trying to fly via GCS and lost connection.  fall back to transmitter */
                UAVObjMetadata metadata;
                ManualControlCommandGetMetadata(&metadata);
                UAVObjSetAccess(&metadata, ACCESS_READWRITE);
                ManualControlCommandSetMetadata(&metadata);
            }
        }

        if (!ManualControlCommandReadOnly()) {
            bool valid_input_detected = true;

            // Read channel values in us
            for (uint8_t n = 0; n < MANUALCONTROLSETTINGS_CHANNELGROUPS_NUMELEM && n < MANUALCONTROLCOMMAND_CHANNEL_NUMELEM; ++n) {
                extern uint32_t pios_rcvr_group_map[];

                if (cast_struct_to_array(settings.ChannelGroups, settings.ChannelGroups.Roll)[n] >= MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE) {
                    cmd.Channel[n] = PIOS_RCVR_INVALID;
                } else {
                    cmd.Channel[n] = PIOS_RCVR_Read(pios_rcvr_group_map[
                                                        cast_struct_to_array(settings.ChannelGroups, settings.ChannelGroups.Pitch)[n]],
                                                    cast_struct_to_array(settings.ChannelNumber, settings.ChannelNumber.Pitch)[n]);
                }

                // If a channel has timed out this is not valid data and we shouldn't update anything
                // until we decide to go to failsafe
                if (cmd.Channel[n] == (uint16_t)PIOS_RCVR_TIMEOUT) {
                    valid_input_detected = false;
                } else {
                    scaledChannel[n] = scaleChannel(cmd.Channel[n],
                                                    cast_struct_to_array(settings.ChannelMax, settings.ChannelMax.Pitch)[n],
                                                    cast_struct_to_array(settings.ChannelMin, settings.ChannelMin.Pitch)[n],
                                                    cast_struct_to_array(settings.ChannelNeutral, settings.ChannelNeutral.Pitch)[n]);
                }
            }

            // Check settings, if error raise alarm
            if (settings.ChannelGroups.Roll >= MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE
                || settings.ChannelGroups.Pitch >= MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE
                || settings.ChannelGroups.Yaw >= MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE
                || settings.ChannelGroups.Throttle >= MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE
                ||
                // Check all channel mappings are valid
                cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_ROLL] == (uint16_t)PIOS_RCVR_INVALID
                || cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_PITCH] == (uint16_t)PIOS_RCVR_INVALID
                || cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_YAW] == (uint16_t)PIOS_RCVR_INVALID
                || cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_THROTTLE] == (uint16_t)PIOS_RCVR_INVALID
                ||
                // Check the driver exists
                cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_ROLL] == (uint16_t)PIOS_RCVR_NODRIVER
                || cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_PITCH] == (uint16_t)PIOS_RCVR_NODRIVER
                || cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_YAW] == (uint16_t)PIOS_RCVR_NODRIVER
                || cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_THROTTLE] == (uint16_t)PIOS_RCVR_NODRIVER ||
                // Check the FlightModeNumber is valid
                settings.FlightModeNumber < 1 || settings.FlightModeNumber > MANUALCONTROLSETTINGS_FLIGHTMODEPOSITION_NUMELEM ||
                // Similar checks for FlightMode channel but only if more than one flight mode has been set. Otherwise don't care
                ((settings.FlightModeNumber > 1)
                 && (settings.ChannelGroups.FlightMode >= MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE
                     || cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_FLIGHTMODE] == (uint16_t)PIOS_RCVR_INVALID
                     || cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_FLIGHTMODE] == (uint16_t)PIOS_RCVR_NODRIVER))) {
                AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_CRITICAL);
                cmd.Connected = MANUALCONTROLCOMMAND_CONNECTED_FALSE;
                ManualControlCommandSet(&cmd);

                // Need to do this here since we don't process armed status.  Since this shouldn't happen in flight (changed config)
                // immediately disarm
                setArmedIfChanged(FLIGHTSTATUS_ARMED_DISARMED);

                continue;
            }

            // decide if we have valid manual input or not
            valid_input_detected &= validInputRange(settings.ChannelMin.Throttle,
                                                    settings.ChannelMax.Throttle, cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_THROTTLE])
                                    && validInputRange(settings.ChannelMin.Roll,
                                                       settings.ChannelMax.Roll, cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_ROLL])
                                    && validInputRange(settings.ChannelMin.Yaw,
                                                       settings.ChannelMax.Yaw, cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_YAW])
                                    && validInputRange(settings.ChannelMin.Pitch,
                                                       settings.ChannelMax.Pitch, cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_PITCH]);

            // Implement hysteresis loop on connection status
            if (valid_input_detected && (++connected_count > 10)) {
                cmd.Connected      = MANUALCONTROLCOMMAND_CONNECTED_TRUE;
                connected_count    = 0;
                disconnected_count = 0;
            } else if (!valid_input_detected && (++disconnected_count > 10)) {
                cmd.Connected      = MANUALCONTROLCOMMAND_CONNECTED_FALSE;
                connected_count    = 0;
                disconnected_count = 0;
            }

            int8_t armSwitch = 0;
            if (cmd.Connected == MANUALCONTROLCOMMAND_CONNECTED_FALSE) {
                cmd.Throttle   = -1;      // Shut down engine with no control
                cmd.Roll       = 0;
                cmd.Yaw = 0;
                cmd.Pitch      = 0;
                cmd.Collective = 0;
                if (settings.FailsafeBehavior != MANUALCONTROLSETTINGS_FAILSAFEBEHAVIOR_NONE) {
                    FlightStatusGet(&flightStatus);

                    cmd.FlightModeSwitchPosition = (uint8_t)settings.FailsafeBehavior - 1;
                    flightStatus.FlightMode = settings.FlightModePosition[settings.FailsafeBehavior - 1];
                    FlightStatusSet(&flightStatus);
                }
                AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_WARNING);

                AccessoryDesiredData accessory;
                // Set Accessory 0
                if (settings.ChannelGroups.Accessory0 != MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE) {
                    accessory.AccessoryVal = 0;
                    if (AccessoryDesiredInstSet(0, &accessory) != 0) {
                        AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_WARNING);
                    }
                }
                // Set Accessory 1
                if (settings.ChannelGroups.Accessory1 != MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE) {
                    accessory.AccessoryVal = 0;
                    if (AccessoryDesiredInstSet(1, &accessory) != 0) {
                        AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_WARNING);
                    }
                }
                // Set Accessory 2
                if (settings.ChannelGroups.Accessory2 != MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE) {
                    accessory.AccessoryVal = 0;
                    if (AccessoryDesiredInstSet(2, &accessory) != 0) {
                        AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_WARNING);
                    }
                }
            } else if (valid_input_detected) {
                AlarmsClear(SYSTEMALARMS_ALARM_MANUALCONTROL);

                // Scale channels to -1 -> +1 range
                cmd.Roll     = scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_ROLL];
                cmd.Pitch    = scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_PITCH];
                cmd.Yaw      = scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_YAW];
                cmd.Throttle = scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_THROTTLE];
                flightMode   = scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_FLIGHTMODE];

                // Apply deadband for Roll/Pitch/Yaw stick inputs
                if (settings.Deadband > 0.0f) {
                    applyDeadband(&cmd.Roll, settings.Deadband);
                    applyDeadband(&cmd.Pitch, settings.Deadband);
                    applyDeadband(&cmd.Yaw, settings.Deadband);
                }
#ifdef USE_INPUT_LPF
                // Apply Low Pass Filter to input channels, time delta between calls in ms
                portTickType thisSysTime = xTaskGetTickCount();
                float dT = (thisSysTime > lastSysTimeLPF) ?
                           (float)((thisSysTime - lastSysTimeLPF) * portTICK_RATE_MS) :
                           (float)UPDATE_PERIOD_MS;
                lastSysTimeLPF = thisSysTime;

                applyLPF(&cmd.Roll, MANUALCONTROLSETTINGS_RESPONSETIME_ROLL, &settings, dT);
                applyLPF(&cmd.Pitch, MANUALCONTROLSETTINGS_RESPONSETIME_PITCH, &settings, dT);
                applyLPF(&cmd.Yaw, MANUALCONTROLSETTINGS_RESPONSETIME_YAW, &settings, dT);
#endif // USE_INPUT_LPF
                if (cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_COLLECTIVE] != (uint16_t)PIOS_RCVR_INVALID
                    && cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_COLLECTIVE] != (uint16_t)PIOS_RCVR_NODRIVER
                    && cmd.Channel[MANUALCONTROLSETTINGS_CHANNELGROUPS_COLLECTIVE] != (uint16_t)PIOS_RCVR_TIMEOUT) {
                    cmd.Collective = scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_COLLECTIVE];
                }

                AccessoryDesiredData accessory;
                // Set Accessory 0
                if (settings.ChannelGroups.Accessory0 != MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE) {
                    accessory.AccessoryVal = scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_ACCESSORY0];
#ifdef USE_INPUT_LPF
                    applyLPF(&accessory.AccessoryVal, MANUALCONTROLSETTINGS_RESPONSETIME_ACCESSORY0, &settings, dT);
#endif
                    if (settings.Arming == MANUALCONTROLSETTINGS_ARMING_ACCESSORY0) {
                        if (accessory.AccessoryVal > ARMED_THRESHOLD) {
                            armSwitch = 1;
                        } else if (accessory.AccessoryVal < -ARMED_THRESHOLD) {
                            armSwitch = -1;
                        }
                    }
                    if (AccessoryDesiredInstSet(0, &accessory) != 0) {
                        AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_WARNING);
                    }
                }
                // Set Accessory 1
                if (settings.ChannelGroups.Accessory1 != MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE) {
                    accessory.AccessoryVal = scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_ACCESSORY1];
#ifdef USE_INPUT_LPF
                    applyLPF(&accessory.AccessoryVal, MANUALCONTROLSETTINGS_RESPONSETIME_ACCESSORY1, &settings, dT);
#endif
                    if (settings.Arming == MANUALCONTROLSETTINGS_ARMING_ACCESSORY1) {
                        if (accessory.AccessoryVal > ARMED_THRESHOLD) {
                            armSwitch = 1;
                        } else if (accessory.AccessoryVal < -ARMED_THRESHOLD) {
                            armSwitch = -1;
                        }
                    }
                    if (AccessoryDesiredInstSet(1, &accessory) != 0) {
                        AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_WARNING);
                    }
                }
                // Set Accessory 2
                if (settings.ChannelGroups.Accessory2 != MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE) {
                    accessory.AccessoryVal = scaledChannel[MANUALCONTROLSETTINGS_CHANNELGROUPS_ACCESSORY2];
#ifdef USE_INPUT_LPF
                    applyLPF(&accessory.AccessoryVal, MANUALCONTROLSETTINGS_RESPONSETIME_ACCESSORY2, &settings, dT);
#endif
                    if (settings.Arming == MANUALCONTROLSETTINGS_ARMING_ACCESSORY2) {
                        if (accessory.AccessoryVal > ARMED_THRESHOLD) {
                            armSwitch = 1;
                        } else if (accessory.AccessoryVal < -ARMED_THRESHOLD) {
                            armSwitch = -1;
                        }
                    }

                    if (AccessoryDesiredInstSet(2, &accessory) != 0) {
                        AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_WARNING);
                    }
                }

                processFlightMode(&settings, flightMode, &cmd);
            }

            // Process arming outside conditional so system will disarm when disconnected
            processArm(&cmd, &settings, armSwitch);

            // Update cmd object
            ManualControlCommandSet(&cmd);
#if defined(PIOS_INCLUDE_USB_RCTX)
            if (pios_usb_rctx_id) {
                PIOS_USB_RCTX_Update(pios_usb_rctx_id,
                                     cmd.Channel,
                                     cast_struct_to_array(settings.ChannelMin, settings.ChannelMin.Roll),
                                     cast_struct_to_array(settings.ChannelMax, settings.ChannelMax.Roll),
                                     NELEMENTS(cmd.Channel));
            }
#endif /* PIOS_INCLUDE_USB_RCTX */
        } else {
            ManualControlCommandGet(&cmd); /* Under GCS control */
        }

        FlightStatusGet(&flightStatus);

        // Depending on the mode update the Stabilization or Actuator objects
        static uint8_t lastFlightMode = FLIGHTSTATUS_FLIGHTMODE_MANUAL;
        switch (PARSE_FLIGHT_MODE(flightStatus.FlightMode)) {
        case FLIGHTMODE_UNDEFINED:
            // This reflects a bug in the code architecture!
            AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_CRITICAL);
            break;
        case FLIGHTMODE_MANUAL:
            updateActuatorDesired(&cmd);
            break;
        case FLIGHTMODE_STABILIZED:
            updateStabilizationDesired(&cmd, &settings);
            break;
        case FLIGHTMODE_TUNING:
            // Tuning takes settings directly from manualcontrolcommand.  No need to
            // call anything else.  This just avoids errors.
            break;
        case FLIGHTMODE_GUIDANCE:
            switch (flightStatus.FlightMode) {
            case FLIGHTSTATUS_FLIGHTMODE_ALTITUDEHOLD:
            case FLIGHTSTATUS_FLIGHTMODE_ALTITUDEVARIO:
                altitudeHoldDesired(&cmd, lastFlightMode != flightStatus.FlightMode);
                break;
            case FLIGHTSTATUS_FLIGHTMODE_POSITIONHOLD:
            case FLIGHTSTATUS_FLIGHTMODE_POI:
                updatePathDesired(&cmd, lastFlightMode != flightStatus.FlightMode, false);
                break;
            case FLIGHTSTATUS_FLIGHTMODE_RETURNTOBASE:
                updatePathDesired(&cmd, lastFlightMode != flightStatus.FlightMode, true);
                break;
            case FLIGHTSTATUS_FLIGHTMODE_PATHPLANNER:
                // No need to call anything.  This just avoids errors.
                break;
            case FLIGHTSTATUS_FLIGHTMODE_LAND:
                updateLandDesired(&cmd, lastFlightMode != flightStatus.FlightMode);
                break;
            default:
                AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_CRITICAL);
            }
            break;
        }
        lastFlightMode = flightStatus.FlightMode;
    }
}

static void resetRcvrActivity(struct rcvr_activity_fsm *fsm)
{
    ReceiverActivityData data;
    bool updated = false;

    /* Clear all channel activity flags */
    ReceiverActivityGet(&data);
    if (data.ActiveGroup != RECEIVERACTIVITY_ACTIVEGROUP_NONE && data.ActiveChannel != 255) {
        data.ActiveGroup   = RECEIVERACTIVITY_ACTIVEGROUP_NONE;
        data.ActiveChannel = 255;
        updated = true;
    }
    if (updated) {
        ReceiverActivitySet(&data);
    }

    /* Reset the FSM state */
    fsm->group = 0;
    fsm->sample_count = 0;
}

static void updateRcvrActivitySample(uint32_t rcvr_id, uint16_t samples[], uint8_t max_channels)
{
    for (uint8_t channel = 1; channel <= max_channels; channel++) {
        // Subtract 1 because channels are 1 indexed
        samples[channel - 1] = PIOS_RCVR_Read(rcvr_id, channel);
    }
}

static bool updateRcvrActivityCompare(uint32_t rcvr_id, struct rcvr_activity_fsm *fsm)
{
    bool activity_updated = false;

    /* Compare the current value to the previous sampled value */
    for (uint8_t channel = 1; channel <= RCVR_ACTIVITY_MONITOR_CHANNELS_PER_GROUP; channel++) {
        uint16_t delta;
        uint16_t prev = fsm->prev[channel - 1]; // Subtract 1 because channels are 1 indexed
        uint16_t curr = PIOS_RCVR_Read(rcvr_id, channel);
        if (curr > prev) {
            delta = curr - prev;
        } else {
            delta = prev - curr;
        }

        if (delta > RCVR_ACTIVITY_MONITOR_MIN_RANGE) {
            /* Mark this channel as active */
            ReceiverActivityActiveGroupOptions group;

            /* Don't assume manualcontrolsettings and receiveractivity are in the same order. */
            switch (fsm->group) {
            case MANUALCONTROLSETTINGS_CHANNELGROUPS_PWM:
                group = RECEIVERACTIVITY_ACTIVEGROUP_PWM;
                break;
            case MANUALCONTROLSETTINGS_CHANNELGROUPS_PPM:
                group = RECEIVERACTIVITY_ACTIVEGROUP_PPM;
                break;
            case MANUALCONTROLSETTINGS_CHANNELGROUPS_DSMMAINPORT:
                group = RECEIVERACTIVITY_ACTIVEGROUP_DSMMAINPORT;
                break;
            case MANUALCONTROLSETTINGS_CHANNELGROUPS_DSMFLEXIPORT:
                group = RECEIVERACTIVITY_ACTIVEGROUP_DSMFLEXIPORT;
                break;
            case MANUALCONTROLSETTINGS_CHANNELGROUPS_SBUS:
                group = RECEIVERACTIVITY_ACTIVEGROUP_SBUS;
                break;
            case MANUALCONTROLSETTINGS_CHANNELGROUPS_GCS:
                group = RECEIVERACTIVITY_ACTIVEGROUP_GCS;
                break;
            case MANUALCONTROLSETTINGS_CHANNELGROUPS_OPLINK:
                group = RECEIVERACTIVITY_ACTIVEGROUP_OPLINK;
                break;
            default:
                PIOS_Assert(0);
                break;
            }

            ReceiverActivityActiveGroupSet((uint8_t *)&group);
            ReceiverActivityActiveChannelSet(&channel);
            activity_updated = true;
        }
    }
    return activity_updated;
}

static bool updateRcvrActivity(struct rcvr_activity_fsm *fsm)
{
    bool activity_updated = false;

    if (fsm->group >= MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE) {
        /* We're out of range, reset things */
        resetRcvrActivity(fsm);
    }

    extern uint32_t pios_rcvr_group_map[];
    if (!pios_rcvr_group_map[fsm->group]) {
        /* Unbound group, skip it */
        goto group_completed;
    }

    if (fsm->sample_count == 0) {
        /* Take a sample of each channel in this group */
        updateRcvrActivitySample(pios_rcvr_group_map[fsm->group], fsm->prev, NELEMENTS(fsm->prev));
        fsm->sample_count++;
        return false;
    }

    /* Compare with previous sample */
    activity_updated = updateRcvrActivityCompare(pios_rcvr_group_map[fsm->group], fsm);

group_completed:
    /* Reset the sample counter */
    fsm->sample_count = 0;

    /* Find the next active group, but limit search so we can't loop forever here */
    for (uint8_t i = 0; i < MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE; i++) {
        /* Move to the next group */
        fsm->group++;
        if (fsm->group >= MANUALCONTROLSETTINGS_CHANNELGROUPS_NONE) {
            /* Wrap back to the first group */
            fsm->group = 0;
        }
        if (pios_rcvr_group_map[fsm->group]) {
            /*
             * Found an active group, take a sample here to avoid an
             * extra 20ms delay in the main thread so we can speed up
             * this algorithm.
             */
            updateRcvrActivitySample(pios_rcvr_group_map[fsm->group], fsm->prev, NELEMENTS(fsm->prev));
            fsm->sample_count++;
            break;
        }
    }

    return activity_updated;
}

static void updateActuatorDesired(ManualControlCommandData *cmd)
{
    ActuatorDesiredData actuator;

    ActuatorDesiredGet(&actuator);
    actuator.Roll     = cmd->Roll;
    actuator.Pitch    = cmd->Pitch;
    actuator.Yaw      = cmd->Yaw;
    actuator.Throttle = (cmd->Throttle < 0) ? -1 : cmd->Throttle;
    ActuatorDesiredSet(&actuator);
}

static void updateStabilizationDesired(ManualControlCommandData *cmd, ManualControlSettingsData *settings)
{
    StabilizationDesiredData stabilization;

    StabilizationDesiredGet(&stabilization);

    StabilizationBankData stabSettings;
    StabilizationBankGet(&stabSettings);

    uint8_t *stab_settings;
    FlightStatusData flightStatus;
    FlightStatusGet(&flightStatus);
    switch (flightStatus.FlightMode) {
    case FLIGHTSTATUS_FLIGHTMODE_STABILIZED1:
        stab_settings = cast_struct_to_array(settings->Stabilization1Settings, settings->Stabilization1Settings.Roll);
        break;
    case FLIGHTSTATUS_FLIGHTMODE_STABILIZED2:
        stab_settings = cast_struct_to_array(settings->Stabilization2Settings, settings->Stabilization2Settings.Roll);
        break;
    case FLIGHTSTATUS_FLIGHTMODE_STABILIZED3:
        stab_settings = cast_struct_to_array(settings->Stabilization3Settings, settings->Stabilization3Settings.Roll);
        break;
    default:
        // Major error, this should not occur because only enter this block when one of these is true
        AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_CRITICAL);
        return;
    }

    stabilization.Roll =
        (stab_settings[0] == STABILIZATIONDESIRED_STABILIZATIONMODE_NONE) ? cmd->Roll :
        (stab_settings[0] == STABILIZATIONDESIRED_STABILIZATIONMODE_RATE) ? cmd->Roll * stabSettings.ManualRate.Roll :
        (stab_settings[0] == STABILIZATIONDESIRED_STABILIZATIONMODE_WEAKLEVELING) ? cmd->Roll * stabSettings.ManualRate.Roll :
        (stab_settings[0] == STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE) ? cmd->Roll * stabSettings.RollMax :
        (stab_settings[0] == STABILIZATIONDESIRED_STABILIZATIONMODE_AXISLOCK) ? cmd->Roll * stabSettings.ManualRate.Roll :
        (stab_settings[0] == STABILIZATIONDESIRED_STABILIZATIONMODE_VIRTUALBAR) ? cmd->Roll :
        (stab_settings[0] == STABILIZATIONDESIRED_STABILIZATIONMODE_RATTITUDE) ? cmd->Roll :
        (stab_settings[0] == STABILIZATIONDESIRED_STABILIZATIONMODE_RELAYRATE) ? cmd->Roll * stabSettings.ManualRate.Roll :
        (stab_settings[0] == STABILIZATIONDESIRED_STABILIZATIONMODE_RELAYATTITUDE) ? cmd->Roll * stabSettings.RollMax :
        0; // this is an invalid mode

    stabilization.Pitch =
        (stab_settings[1] == STABILIZATIONDESIRED_STABILIZATIONMODE_NONE) ? cmd->Pitch :
        (stab_settings[1] == STABILIZATIONDESIRED_STABILIZATIONMODE_RATE) ? cmd->Pitch * stabSettings.ManualRate.Pitch :
        (stab_settings[1] == STABILIZATIONDESIRED_STABILIZATIONMODE_WEAKLEVELING) ? cmd->Pitch * stabSettings.ManualRate.Pitch :
        (stab_settings[1] == STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE) ? cmd->Pitch * stabSettings.PitchMax :
        (stab_settings[1] == STABILIZATIONDESIRED_STABILIZATIONMODE_AXISLOCK) ? cmd->Pitch * stabSettings.ManualRate.Pitch :
        (stab_settings[1] == STABILIZATIONDESIRED_STABILIZATIONMODE_VIRTUALBAR) ? cmd->Pitch :
        (stab_settings[1] == STABILIZATIONDESIRED_STABILIZATIONMODE_RATTITUDE) ? cmd->Pitch :
        (stab_settings[1] == STABILIZATIONDESIRED_STABILIZATIONMODE_RELAYRATE) ? cmd->Pitch * stabSettings.ManualRate.Pitch :
        (stab_settings[1] == STABILIZATIONDESIRED_STABILIZATIONMODE_RELAYATTITUDE) ? cmd->Pitch * stabSettings.PitchMax :
        0; // this is an invalid mode

    // TOOD: Add assumption about order of stabilization desired and manual control stabilization mode fields having same order
    stabilization.StabilizationMode.Roll  = stab_settings[0];
    stabilization.StabilizationMode.Pitch = stab_settings[1];
    // Other axes (yaw) cannot be Rattitude, so use Rate
    // Should really do this for Attitude mode as well?
    if (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_RATTITUDE) {
        stabilization.StabilizationMode.Yaw = STABILIZATIONDESIRED_STABILIZATIONMODE_RATE;
        stabilization.Yaw = cmd->Yaw * stabSettings.ManualRate.Yaw;
    } else {
        stabilization.StabilizationMode.Yaw = stab_settings[2];
        stabilization.Yaw =
            (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_NONE) ? cmd->Yaw :
            (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_RATE) ? cmd->Yaw * stabSettings.ManualRate.Yaw :
            (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_WEAKLEVELING) ? cmd->Yaw * stabSettings.ManualRate.Yaw :
            (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_ATTITUDE) ? cmd->Yaw * stabSettings.YawMax :
            (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_AXISLOCK) ? cmd->Yaw * stabSettings.ManualRate.Yaw :
            (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_VIRTUALBAR) ? cmd->Yaw :
            (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_RATTITUDE) ? cmd->Yaw :
            (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_RELAYRATE) ? cmd->Yaw * stabSettings.ManualRate.Yaw :
            (stab_settings[2] == STABILIZATIONDESIRED_STABILIZATIONMODE_RELAYATTITUDE) ? cmd->Yaw * stabSettings.YawMax :
            0; // this is an invalid mode
    }

    stabilization.Throttle = (cmd->Throttle < 0) ? -1 : cmd->Throttle;
    StabilizationDesiredSet(&stabilization);
}

#if defined(REVOLUTION)
// TODO: Need compile flag to exclude this from copter control
/**
 * @brief Update the position desired to current location when
 * enabled and allow the waypoint to be moved by transmitter
 */
static void updatePathDesired(__attribute__((unused)) ManualControlCommandData *cmd, bool changed, bool home)
{
    /*
       static portTickType lastSysTime;
       portTickType thisSysTime = xTaskGetTickCount();
       dT = ((thisSysTime == lastSysTime)? 0.001f : (thisSysTime - lastSysTime) * portTICK_RATE_MS * 0.001f);
       lastSysTime = thisSysTime;
     */

    if (home && changed) {
        // Simple Return To Base mode - keep altitude the same, fly to home position
        PositionStateData positionState;
        PositionStateGet(&positionState);
        ManualControlSettingsData settings;
        ManualControlSettingsGet(&settings);

        PathDesiredData pathDesired;
        PathDesiredGet(&pathDesired);
        pathDesired.Start.North      = 0;
        pathDesired.Start.East       = 0;
        pathDesired.Start.Down       = positionState.Down - settings.ReturnToHomeAltitudeOffset;
        pathDesired.End.North        = 0;
        pathDesired.End.East         = 0;
        pathDesired.End.Down         = positionState.Down - settings.ReturnToHomeAltitudeOffset;
        pathDesired.StartingVelocity = 1;
        pathDesired.EndingVelocity   = 0;
        pathDesired.Mode = PATHDESIRED_MODE_FLYENDPOINT;
        PathDesiredSet(&pathDesired);
    } else if (changed) {
        // After not being in this mode for a while init at current height
        PositionStateData positionState;
        PositionStateGet(&positionState);

        PathDesiredData pathDesired;
        PathDesiredGet(&pathDesired);
        pathDesired.Start.North      = positionState.North;
        pathDesired.Start.East       = positionState.East;
        pathDesired.Start.Down       = positionState.Down;
        pathDesired.End.North        = positionState.North;
        pathDesired.End.East         = positionState.East;
        pathDesired.End.Down         = positionState.Down;
        pathDesired.StartingVelocity = 1;
        pathDesired.EndingVelocity   = 0;
        pathDesired.Mode = PATHDESIRED_MODE_FLYENDPOINT;
        PathDesiredSet(&pathDesired);
        /* Disable this section, until such time as proper discussion can be had about how to implement it for all types of crafts.
           } else {
           PathDesiredData pathDesired;
           PathDesiredGet(&pathDesired);
           pathDesired.End[PATHDESIRED_END_NORTH] += dT * -cmd->Pitch;
           pathDesired.End[PATHDESIRED_END_EAST] += dT * cmd->Roll;
           pathDesired.Mode = PATHDESIRED_MODE_FLYENDPOINT;
           PathDesiredSet(&pathDesired);
         */
    }
}

static void updateLandDesired(__attribute__((unused)) ManualControlCommandData *cmd, bool changed)
{
    /*
       static portTickType lastSysTime;
       portTickType thisSysTime;
       float dT;

       thisSysTime = xTaskGetTickCount();
        dT = ((thisSysTime == lastSysTime)? 0.001f : (thisSysTime - lastSysTime) * portTICK_RATE_MS * 0.001f);
       lastSysTime = thisSysTime;
     */

    PositionStateData positionState;

    PositionStateGet(&positionState);

    PathDesiredData pathDesired;
    PathDesiredGet(&pathDesired);
    if (changed) {
        // After not being in this mode for a while init at current height
        pathDesired.Start.North      = positionState.North;
        pathDesired.Start.East       = positionState.East;
        pathDesired.Start.Down       = positionState.Down;
        pathDesired.End.North        = positionState.North;
        pathDesired.End.East         = positionState.East;
        pathDesired.End.Down         = positionState.Down;
        pathDesired.StartingVelocity = 1;
        pathDesired.EndingVelocity   = 0;
        pathDesired.Mode = PATHDESIRED_MODE_FLYENDPOINT;
    }
    pathDesired.End.Down = positionState.Down + 5;
    PathDesiredSet(&pathDesired);
}

/**
 * @brief Update the altitude desired to current altitude when
 * enabled and enable altitude mode for stabilization
 * @todo: Need compile flag to exclude this from copter control
 */
static void altitudeHoldDesired(ManualControlCommandData *cmd, bool changed)
{
    const float DEADBAND      = 0.20f;
    const float DEADBAND_HIGH = 1.0f / 2 + DEADBAND / 2;
    const float DEADBAND_LOW  = 1.0f / 2 - DEADBAND / 2;

    // this is the max speed in m/s at the extents of throttle
    float throttleRate;
    uint8_t throttleExp;

    static uint8_t flightMode;
    static bool newaltitude = true;

    FlightStatusFlightModeGet(&flightMode);

    AltitudeHoldDesiredData altitudeHoldDesiredData;
    AltitudeHoldDesiredGet(&altitudeHoldDesiredData);

    AltitudeHoldSettingsThrottleExpGet(&throttleExp);
    AltitudeHoldSettingsThrottleRateGet(&throttleRate);

    StabilizationBankData stabSettings;
    StabilizationBankGet(&stabSettings);

    PositionStateData posState;
    PositionStateGet(&posState);

    altitudeHoldDesiredData.Roll  = cmd->Roll * stabSettings.RollMax;
    altitudeHoldDesiredData.Pitch = cmd->Pitch * stabSettings.PitchMax;
    altitudeHoldDesiredData.Yaw   = cmd->Yaw * stabSettings.ManualRate.Yaw;

    if (changed) {
        newaltitude = true;
    }

    uint8_t cutOff;
    AltitudeHoldSettingsCutThrottleWhenZeroGet(&cutOff);
    if (cutOff && cmd->Throttle < 0) {
        // Cut throttle if desired
        altitudeHoldDesiredData.SetPoint    = cmd->Throttle;
        altitudeHoldDesiredData.ControlMode = ALTITUDEHOLDDESIRED_CONTROLMODE_THROTTLE;
        newaltitude = true;
    } else if (flightMode == FLIGHTSTATUS_FLIGHTMODE_ALTITUDEVARIO && cmd->Throttle > DEADBAND_HIGH) {
        // being the two band symmetrical I can divide by DEADBAND_LOW to scale it to a value betweeon 0 and 1
        // then apply an "exp" f(x,k) = (k*x*x*x + (255-k)*x) / 255
        altitudeHoldDesiredData.SetPoint    = -((throttleExp * powf((cmd->Throttle - DEADBAND_HIGH) / (DEADBAND_LOW), 3) + (255 - throttleExp) * (cmd->Throttle - DEADBAND_HIGH) / DEADBAND_LOW) / 255 * throttleRate);
        altitudeHoldDesiredData.ControlMode = ALTITUDEHOLDDESIRED_CONTROLMODE_VELOCITY;
        newaltitude = true;
    } else if (flightMode == FLIGHTSTATUS_FLIGHTMODE_ALTITUDEVARIO && cmd->Throttle < DEADBAND_LOW) {
        altitudeHoldDesiredData.SetPoint    = -(-(throttleExp * powf((DEADBAND_LOW - (cmd->Throttle < 0 ? 0 : cmd->Throttle)) / DEADBAND_LOW, 3) + (255 - throttleExp) * (DEADBAND_LOW - cmd->Throttle) / DEADBAND_LOW) / 255 * throttleRate);
        altitudeHoldDesiredData.ControlMode = ALTITUDEHOLDDESIRED_CONTROLMODE_VELOCITY;
        newaltitude = true;
    } else if (newaltitude == true) {
        altitudeHoldDesiredData.SetPoint    = posState.Down;
        altitudeHoldDesiredData.ControlMode = ALTITUDEHOLDDESIRED_CONTROLMODE_ALTITUDE;
        newaltitude = false;
    }

    AltitudeHoldDesiredSet(&altitudeHoldDesiredData);
}
#else /* if defined(REVOLUTION) */

// TODO: These functions should never be accessible on CC.  Any configuration that
// could allow them to be called should already throw an error to prevent this happening
// in flight
static void updatePathDesired(__attribute__((unused)) ManualControlCommandData *cmd,
                              __attribute__((unused)) bool changed,
                              __attribute__((unused)) bool home)
{
    AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_ERROR);
}

static void updateLandDesired(__attribute__((unused)) ManualControlCommandData *cmd,
                              __attribute__((unused)) bool changed)
{
    AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_ERROR);
}

static void altitudeHoldDesired(__attribute__((unused)) ManualControlCommandData *cmd,
                                __attribute__((unused)) bool changed)
{
    AlarmsSet(SYSTEMALARMS_ALARM_MANUALCONTROL, SYSTEMALARMS_ALARM_ERROR);
}
#endif /* if defined(REVOLUTION) */
/**
 * Convert channel from servo pulse duration (microseconds) to scaled -1/+1 range.
 */
static float scaleChannel(int16_t value, int16_t max, int16_t min, int16_t neutral)
{
    float valueScaled;

    // Scale
    if ((max > min && value >= neutral) || (min > max && value <= neutral)) {
        if (max != neutral) {
            valueScaled = (float)(value - neutral) / (float)(max - neutral);
        } else {
            valueScaled = 0;
        }
    } else {
        if (min != neutral) {
            valueScaled = (float)(value - neutral) / (float)(neutral - min);
        } else {
            valueScaled = 0;
        }
    }

    // Bound
    if (valueScaled > 1.0f) {
        valueScaled = 1.0f;
    } else if (valueScaled < -1.0f) {
        valueScaled = -1.0f;
    }

    return valueScaled;
}

static uint32_t timeDifferenceMs(portTickType start_time, portTickType end_time)
{
    return (end_time - start_time) * portTICK_RATE_MS;
}

/**
 * @brief Determine if the aircraft is safe to arm
 * @returns True if safe to arm, false otherwise
 */
static bool okToArm(void)
{
    // update checks
    configuration_check();

    // read alarms
    SystemAlarmsData alarms;

    SystemAlarmsGet(&alarms);

    // Check each alarm
    for (int i = 0; i < SYSTEMALARMS_ALARM_NUMELEM; i++) {
        if (cast_struct_to_array(alarms.Alarm, alarms.Alarm.Actuator)[i] >= SYSTEMALARMS_ALARM_ERROR) { // found an alarm thats set
            if (i == SYSTEMALARMS_ALARM_GPS || i == SYSTEMALARMS_ALARM_TELEMETRY) {
                continue;
            }

            return false;
        }
    }

    uint8_t flightMode;
    FlightStatusFlightModeGet(&flightMode);
    switch (flightMode) {
    case FLIGHTSTATUS_FLIGHTMODE_MANUAL:
    case FLIGHTSTATUS_FLIGHTMODE_STABILIZED1:
    case FLIGHTSTATUS_FLIGHTMODE_STABILIZED2:
    case FLIGHTSTATUS_FLIGHTMODE_STABILIZED3:
        return true;

    default:
        return false;
    }
}
/**
 * @brief Determine if the aircraft is forced to disarm by an explicit alarm
 * @returns True if safe to arm, false otherwise
 */
static bool forcedDisArm(void)
{
    // read alarms
    SystemAlarmsData alarms;

    SystemAlarmsGet(&alarms);

    if (alarms.Alarm.Guidance == SYSTEMALARMS_ALARM_CRITICAL) {
        return true;
    }
    return false;
}

/**
 * @brief Update the flightStatus object only if value changed.  Reduces callbacks
 * @param[in] val The new value
 */
static void setArmedIfChanged(uint8_t val)
{
    FlightStatusData flightStatus;

    FlightStatusGet(&flightStatus);

    if (flightStatus.Armed != val) {
        flightStatus.Armed = val;
        FlightStatusSet(&flightStatus);
    }
}

/**
 * @brief Process the inputs and determine whether to arm or not
 * @param[out] cmd The structure to set the armed in
 * @param[in] settings Settings indicating the necessary position
 */
static void processArm(ManualControlCommandData *cmd, ManualControlSettingsData *settings, int8_t armSwitch)
{
    bool lowThrottle = cmd->Throttle < 0;

    /**
     * do NOT check throttle if disarming via switch, must be instant
     */
    switch (settings->Arming) {
    case MANUALCONTROLSETTINGS_ARMING_ACCESSORY0:
    case MANUALCONTROLSETTINGS_ARMING_ACCESSORY1:
    case MANUALCONTROLSETTINGS_ARMING_ACCESSORY2:
        if (armSwitch < 0) {
            lowThrottle = true;
        }
        break;
    default:
        break;
    }

    if (forcedDisArm()) {
        // PathPlanner forces explicit disarming due to error condition (crash, impact, fire, ...)
        setArmedIfChanged(FLIGHTSTATUS_ARMED_DISARMED);
        return;
    }

    if (settings->Arming == MANUALCONTROLSETTINGS_ARMING_ALWAYSDISARMED) {
        // In this configuration we always disarm
        setArmedIfChanged(FLIGHTSTATUS_ARMED_DISARMED);
    } else {
        // Not really needed since this function not called when disconnected
        if (cmd->Connected == MANUALCONTROLCOMMAND_CONNECTED_FALSE) {
            lowThrottle = true;
        }

        // The throttle is not low, in case we where arming or disarming, abort
        if (!lowThrottle) {
            switch (armState) {
            case ARM_STATE_DISARMING_MANUAL:
            case ARM_STATE_DISARMING_TIMEOUT:
                armState = ARM_STATE_ARMED;
                break;
            case ARM_STATE_ARMING_MANUAL:
                armState = ARM_STATE_DISARMED;
                break;
            default:
                // Nothing needs to be done in the other states
                break;
            }
            return;
        }

        // The rest of these cases throttle is low
        if (settings->Arming == MANUALCONTROLSETTINGS_ARMING_ALWAYSARMED) {
            // In this configuration, we go into armed state as soon as the throttle is low, never disarm
            setArmedIfChanged(FLIGHTSTATUS_ARMED_ARMED);
            return;
        }

        // When the configuration is not "Always armed" and no "Always disarmed",
        // the state will not be changed when the throttle is not low
        static portTickType armedDisarmStart;
        float armingInputLevel = 0;

        // Calc channel see assumptions7
        switch (settings->Arming) {
        case MANUALCONTROLSETTINGS_ARMING_ROLLLEFT:
            armingInputLevel = 1.0f * cmd->Roll;
            break;
        case MANUALCONTROLSETTINGS_ARMING_ROLLRIGHT:
            armingInputLevel = -1.0f * cmd->Roll;
            break;
        case MANUALCONTROLSETTINGS_ARMING_PITCHFORWARD:
            armingInputLevel = 1.0f * cmd->Pitch;
            break;
        case MANUALCONTROLSETTINGS_ARMING_PITCHAFT:
            armingInputLevel = -1.0f * cmd->Pitch;
            break;
        case MANUALCONTROLSETTINGS_ARMING_YAWLEFT:
            armingInputLevel = 1.0f * cmd->Yaw;
            break;
        case MANUALCONTROLSETTINGS_ARMING_YAWRIGHT:
            armingInputLevel = -1.0f * cmd->Yaw;
            break;
        case MANUALCONTROLSETTINGS_ARMING_ACCESSORY0:
        case MANUALCONTROLSETTINGS_ARMING_ACCESSORY1:
        case MANUALCONTROLSETTINGS_ARMING_ACCESSORY2:
            armingInputLevel = -1.0f * (float)armSwitch;
            break;
        }

        bool manualArm    = false;
        bool manualDisarm = false;

        if (armingInputLevel <= -ARMED_THRESHOLD) {
            manualArm = true;
        } else if (armingInputLevel >= +ARMED_THRESHOLD) {
            manualDisarm = true;
        }

        switch (armState) {
        case ARM_STATE_DISARMED:
            setArmedIfChanged(FLIGHTSTATUS_ARMED_DISARMED);

            // only allow arming if it's OK too
            if (manualArm && okToArm()) {
                armedDisarmStart = lastSysTime;
                armState = ARM_STATE_ARMING_MANUAL;
            }
            break;

        case ARM_STATE_ARMING_MANUAL:
            setArmedIfChanged(FLIGHTSTATUS_ARMED_ARMING);

            if (manualArm && (timeDifferenceMs(armedDisarmStart, lastSysTime) > settings->ArmingSequenceTime)) {
                armState = ARM_STATE_ARMED;
            } else if (!manualArm) {
                armState = ARM_STATE_DISARMED;
            }
            break;

        case ARM_STATE_ARMED:
            // When we get here, the throttle is low,
            // we go immediately to disarming due to timeout, also when the disarming mechanism is not enabled
            armedDisarmStart = lastSysTime;
            armState = ARM_STATE_DISARMING_TIMEOUT;
            setArmedIfChanged(FLIGHTSTATUS_ARMED_ARMED);
            break;

        case ARM_STATE_DISARMING_TIMEOUT:
            // We get here when armed while throttle low, even when the arming timeout is not enabled
            if ((settings->ArmedTimeout != 0) && (timeDifferenceMs(armedDisarmStart, lastSysTime) > settings->ArmedTimeout)) {
                armState = ARM_STATE_DISARMED;
            }

            // Switch to disarming due to manual control when needed
            if (manualDisarm) {
                armedDisarmStart = lastSysTime;
                armState = ARM_STATE_DISARMING_MANUAL;
            }
            break;

        case ARM_STATE_DISARMING_MANUAL:
            if (manualDisarm && (timeDifferenceMs(armedDisarmStart, lastSysTime) > settings->DisarmingSequenceTime)) {
                armState = ARM_STATE_DISARMED;
            } else if (!manualDisarm) {
                armState = ARM_STATE_ARMED;
            }
            break;
        } // End Switch
    }
}

/**
 * @brief Determine which of N positions the flight mode switch is in and set flight mode accordingly
 * @param[out] cmd Pointer to the command structure to set the flight mode in
 * @param[in] settings The settings which indicate which position is which mode
 * @param[in] flightMode the value of the switch position
 */
static void processFlightMode(ManualControlSettingsData *settings, float flightMode, ManualControlCommandData *cmd)
{
    FlightStatusData flightStatus;

    FlightStatusGet(&flightStatus);

    // Convert flightMode value into the switch position in the range [0..N-1]
    uint8_t pos = ((int16_t)(flightMode * 256.0f) + 256) * settings->FlightModeNumber >> 9;
    if (pos >= settings->FlightModeNumber) {
        pos = settings->FlightModeNumber - 1;
    }

    cmd->FlightModeSwitchPosition = pos;

    uint8_t newMode = settings->FlightModePosition[pos];

    if (flightStatus.FlightMode != newMode) {
        flightStatus.FlightMode = newMode;
        FlightStatusSet(&flightStatus);
    }
}

/**
 * @brief Determine if the manual input value is within acceptable limits
 * @returns return TRUE if so, otherwise return FALSE
 */
bool validInputRange(int16_t min, int16_t max, uint16_t value)
{
    if (min > max) {
        int16_t tmp = min;
        min = max;
        max = tmp;
    }
    return value >= min - CONNECTION_OFFSET && value <= max + CONNECTION_OFFSET;
}

/**
 * @brief Apply deadband to Roll/Pitch/Yaw channels
 */
static void applyDeadband(float *value, float deadband)
{
    if (fabsf(*value) < deadband) {
        *value = 0.0f;
    } else if (*value > 0.0f) {
        *value -= deadband;
    } else {
        *value += deadband;
    }
}

#ifdef USE_INPUT_LPF
/**
 * @brief Apply Low Pass Filter to Throttle/Roll/Pitch/Yaw or Accessory channel
 */
static void applyLPF(float *value, ManualControlSettingsResponseTimeElem channel, ManualControlSettingsData *settings, float dT)
{
    if (cast_struct_to_array(settings->ResponseTime, settings->ResponseTime.Roll)[channel]) {
        float rt = (float)cast_struct_to_array(settings->ResponseTime, settings->ResponseTime.Roll)[channel];
        inputFiltered[channel] = ((rt * inputFiltered[channel]) + (dT * (*value))) / (rt + dT);
        *value = inputFiltered[channel];
    }
}
#endif // USE_INPUT_LPF
/**
 * Called whenever a critical configuration component changes
 */
static void configurationUpdatedCb(__attribute__((unused)) UAVObjEvent *ev)
{
    configuration_check();
}

/**
 * @}
 * @}
 */
