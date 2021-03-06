/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Sensors
 * @brief Acquires sensor data
 * Specifically updates the the @ref GyroSensor, @ref AccelSensor, and @ref MagSensor objects
 * @{
 *
 * @file       sensors.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
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
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref GyroSensor @ref AccelSensor @ref MagSensor
 *
 * The module executes in its own thread.
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

#include <openpilot.h>

#include <homelocation.h>
#include <magsensor.h>
#include <accelsensor.h>
#include <gyrosensor.h>
#include <attitudestate.h>
#include <attitudesettings.h>
#include <revocalibration.h>
#include <flightstatus.h>
#include <taskinfo.h>

#include <CoordinateConversions.h>

#include <pios_board_info.h>

// Private constants
#define STACK_SIZE_BYTES 1000
#define TASK_PRIORITY    (tskIDLE_PRIORITY + 3)
#define SENSOR_PERIOD    2

// Private types


// Private functions
static void SensorsTask(void *parameters);
static void settingsUpdatedCb(UAVObjEvent *objEv);
// static void magOffsetEstimation(MagSensorData *mag);

// Private variables
static xTaskHandle sensorsTaskHandle;
RevoCalibrationData cal;

// These values are initialized by settings but can be updated by the attitude algorithm

static float mag_bias[3] = { 0, 0, 0 };
static float mag_scale[3] = { 0, 0, 0 };
static float accel_bias[3] = { 0, 0, 0 };
static float accel_scale[3] = { 0, 0, 0 };
static float gyro_staticbias[3] = { 0, 0, 0 };
static float gyro_scale[3] = { 0, 0, 0 };

static float R[3][3] = {
    { 0 }
};
static int8_t rotate = 0;

/**
 * API for sensor fusion algorithms:
 * Configure(xQueueHandle gyro, xQueueHandle accel, xQueueHandle mag, xQueueHandle baro)
 *   Stores all the queues the algorithm will pull data from
 * FinalizeSensors() -- before saving the sensors modifies them based on internal state (gyro bias)
 * Update() -- queries queues and updates the attitude estiamte
 */


/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsInitialize(void)
{
    GyroSensorInitialize();
    AccelSensorInitialize();
    MagSensorInitialize();
    RevoCalibrationInitialize();
    AttitudeSettingsInitialize();

    rotate = 0;

    RevoCalibrationConnectCallback(&settingsUpdatedCb);
    AttitudeSettingsConnectCallback(&settingsUpdatedCb);

    return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t SensorsStart(void)
{
    // Start main task
    xTaskCreate(SensorsTask, (signed char *)"Sensors", STACK_SIZE_BYTES / 4, NULL, TASK_PRIORITY, &sensorsTaskHandle);
    PIOS_TASK_MONITOR_RegisterTask(TASKINFO_RUNNING_SENSORS, sensorsTaskHandle);
#ifdef PIOS_INCLUDE_WDG
    PIOS_WDG_RegisterFlag(PIOS_WDG_SENSORS);
#endif

    return 0;
}

MODULE_INITCALL(SensorsInitialize, SensorsStart);

int32_t accel_test;
int32_t gyro_test;
int32_t mag_test;
// int32_t pressure_test;


/**
 * The sensor task.  This polls the gyros at 500 Hz and pumps that data to
 * stabilization and to the attitude loop
 *
 * This function has a lot of if/defs right now to allow these configurations:
 * 1. BMA180 accel and MPU6000 gyro
 * 2. MPU6000 gyro and accel
 * 3. BMA180 accel and L3GD20 gyro
 */

uint32_t sensor_dt_us;
static void SensorsTask(__attribute__((unused)) void *parameters)
{
    portTickType lastSysTime;
    uint32_t accel_samples = 0;
    uint32_t gyro_samples  = 0;
    int32_t accel_accum[3] = { 0, 0, 0 };
    int32_t gyro_accum[3]  = { 0, 0, 0 };
    float gyro_scaling     = 0;
    float accel_scaling    = 0;
    static int32_t timeval;

    AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);

    UAVObjEvent ev;
    settingsUpdatedCb(&ev);

    const struct pios_board_info *bdinfo = &pios_board_info_blob;

    switch (bdinfo->board_rev) {
    case 0x01:
#if defined(PIOS_INCLUDE_L3GD20)
        gyro_test  = PIOS_L3GD20_Test();
#endif
#if defined(PIOS_INCLUDE_BMA180)
        accel_test = PIOS_BMA180_Test();
#endif
        break;
    case 0x02:
#if defined(PIOS_INCLUDE_MPU6000)
        gyro_test  = PIOS_MPU6000_Test();
        accel_test = gyro_test;
#endif
        break;
    default:
        PIOS_DEBUG_Assert(0);
    }

#if defined(PIOS_INCLUDE_HMC5883)
    mag_test = PIOS_HMC5883_Test();
#else
    mag_test = 0;
#endif

    if (accel_test < 0 || gyro_test < 0 || mag_test < 0) {
        AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
        while (1) {
#ifdef PIOS_INCLUDE_WDG
            PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);
#endif
            vTaskDelay(10);
        }
    }

    // Main task loop
    lastSysTime = xTaskGetTickCount();
    bool error = false;
    uint32_t mag_update_time = PIOS_DELAY_GetRaw();
    while (1) {
        // TODO: add timeouts to the sensor reads and set an error if the fail
        sensor_dt_us = PIOS_DELAY_DiffuS(timeval);
        timeval = PIOS_DELAY_GetRaw();

        if (error) {
#ifdef PIOS_INCLUDE_WDG
            PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);
#endif
            lastSysTime = xTaskGetTickCount();
            vTaskDelayUntil(&lastSysTime, SENSOR_PERIOD / portTICK_RATE_MS);
            AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_CRITICAL);
            error = false;
        } else {
            AlarmsClear(SYSTEMALARMS_ALARM_SENSORS);
        }


        for (int i = 0; i < 3; i++) {
            accel_accum[i] = 0;
            gyro_accum[i]  = 0;
        }
        accel_samples = 0;
        gyro_samples  = 0;

        AccelSensorData accelSensorData;
        GyroSensorData gyroSensorData;

        switch (bdinfo->board_rev) {
        case 0x01: // L3GD20 + BMA180 board
#if defined(PIOS_INCLUDE_BMA180)
            {
                struct pios_bma180_data accel;

                int32_t read_good;
                int32_t count;

                count = 0;
                while ((read_good = PIOS_BMA180_ReadFifo(&accel)) != 0 && !error) {
                    error = ((xTaskGetTickCount() - lastSysTime) > SENSOR_PERIOD) ? true : error;
                }
                if (error) {
                    // Unfortunately if the BMA180 ever misses getting read, then it will not
                    // trigger more interrupts.  In this case we must force a read to kickstarts
                    // it.
                    struct pios_bma180_data data;
                    PIOS_BMA180_ReadAccels(&data);
                    continue;
                }
                while (read_good == 0) {
                    count++;

                    accel_accum[1] += accel.x;
                    accel_accum[0] += accel.y;
                    accel_accum[2] -= accel.z;

                    read_good = PIOS_BMA180_ReadFifo(&accel);
                }
                accel_samples = count;
                accel_scaling = PIOS_BMA180_GetScale();

                // Get temp from last reading
                accelSensorData.temperature = 25.0f + ((float)accel.temperature - 2.0f) / 2.0f;
            }
#endif /* if defined(PIOS_INCLUDE_BMA180) */
#if defined(PIOS_INCLUDE_L3GD20)
            {
                struct pios_l3gd20_data gyro;
                gyro_samples = 0;
                xQueueHandle gyro_queue = PIOS_L3GD20_GetQueue();

                if (xQueueReceive(gyro_queue, (void *)&gyro, 4) == errQUEUE_EMPTY) {
                    error = true;
                    continue;
                }

                gyro_samples   = 1;
                gyro_accum[1] += gyro.gyro_x;
                gyro_accum[0] += gyro.gyro_y;
                gyro_accum[2] -= gyro.gyro_z;

                gyro_scaling   = PIOS_L3GD20_GetScale();

                // Get temp from last reading
                gyroSensorData.temperature = gyro.temperature;
            }
#endif /* if defined(PIOS_INCLUDE_L3GD20) */
            break;
        case 0x02: // MPU6000 board
        case 0x03: // MPU6000 board
#if defined(PIOS_INCLUDE_MPU6000)
            {
                struct pios_mpu6000_data mpu6000_data;
                xQueueHandle queue = PIOS_MPU6000_GetQueue();

                while (xQueueReceive(queue, (void *)&mpu6000_data, gyro_samples == 0 ? 10 : 0) != errQUEUE_EMPTY) {
                    gyro_accum[0]  += mpu6000_data.gyro_x;
                    gyro_accum[1]  += mpu6000_data.gyro_y;
                    gyro_accum[2]  += mpu6000_data.gyro_z;

                    accel_accum[0] += mpu6000_data.accel_x;
                    accel_accum[1] += mpu6000_data.accel_y;
                    accel_accum[2] += mpu6000_data.accel_z;

                    gyro_samples++;
                    accel_samples++;
                }

                if (gyro_samples == 0) {
                    PIOS_MPU6000_ReadGyros(&mpu6000_data);
                    error = true;
                    continue;
                }

                gyro_scaling  = PIOS_MPU6000_GetScale();
                accel_scaling = PIOS_MPU6000_GetAccelScale();

                gyroSensorData.temperature  = 35.0f + ((float)mpu6000_data.temperature + 512.0f) / 340.0f;
                accelSensorData.temperature = 35.0f + ((float)mpu6000_data.temperature + 512.0f) / 340.0f;
            }
#endif /* PIOS_INCLUDE_MPU6000 */
            break;
        default:
            PIOS_DEBUG_Assert(0);
        }

        // Scale the accels
        float accels[3]     = { (float)accel_accum[0] / accel_samples,
                                (float)accel_accum[1] / accel_samples,
                                (float)accel_accum[2] / accel_samples };
        float accels_out[3] = { accels[0] * accel_scaling * accel_scale[0] - accel_bias[0],
                                accels[1] * accel_scaling * accel_scale[1] - accel_bias[1],
                                accels[2] * accel_scaling * accel_scale[2] - accel_bias[2] };
        if (rotate) {
            rot_mult(R, accels_out, accels);
            accelSensorData.x = accels[0];
            accelSensorData.y = accels[1];
            accelSensorData.z = accels[2];
        } else {
            accelSensorData.x = accels_out[0];
            accelSensorData.y = accels_out[1];
            accelSensorData.z = accels_out[2];
        }
        AccelSensorSet(&accelSensorData);

        // Scale the gyros
        float gyros[3]     = { (float)gyro_accum[0] / gyro_samples,
                               (float)gyro_accum[1] / gyro_samples,
                               (float)gyro_accum[2] / gyro_samples };
        float gyros_out[3] = { gyros[0] * gyro_scaling * gyro_scale[0] - gyro_staticbias[0],
                               gyros[1] * gyro_scaling * gyro_scale[1] - gyro_staticbias[1],
                               gyros[2] * gyro_scaling * gyro_scale[2] - gyro_staticbias[2] };
        if (rotate) {
            rot_mult(R, gyros_out, gyros);
            gyroSensorData.x = gyros[0];
            gyroSensorData.y = gyros[1];
            gyroSensorData.z = gyros[2];
        } else {
            gyroSensorData.x = gyros_out[0];
            gyroSensorData.y = gyros_out[1];
            gyroSensorData.z = gyros_out[2];
        }

        GyroSensorSet(&gyroSensorData);

        // Because most crafts wont get enough information from gravity to zero yaw gyro, we try
        // and make it average zero (weakly)

#if defined(PIOS_INCLUDE_HMC5883)
        MagSensorData mag;
        if (PIOS_HMC5883_NewDataAvailable() || PIOS_DELAY_DiffuS(mag_update_time) > 150000) {
            int16_t values[3];
            PIOS_HMC5883_ReadMag(values);
            float mags[3] = { (float)values[1] * mag_scale[0] - mag_bias[0],
                              (float)values[0] * mag_scale[1] - mag_bias[1],
                              -(float)values[2] * mag_scale[2] - mag_bias[2] };
            if (rotate) {
                float mag_out[3];
                rot_mult(R, mags, mag_out);
                mag.x = mag_out[0];
                mag.y = mag_out[1];
                mag.z = mag_out[2];
            } else {
                mag.x = mags[0];
                mag.y = mags[1];
                mag.z = mags[2];
            }

            MagSensorSet(&mag);
            mag_update_time = PIOS_DELAY_GetRaw();
        }
#endif /* if defined(PIOS_INCLUDE_HMC5883) */

#ifdef PIOS_INCLUDE_WDG
        PIOS_WDG_UpdateFlag(PIOS_WDG_SENSORS);
#endif

        lastSysTime = xTaskGetTickCount();
    }
}

/**
 * Locally cache some variables from the AtttitudeSettings object
 */
static void settingsUpdatedCb(__attribute__((unused)) UAVObjEvent *objEv)
{
    RevoCalibrationGet(&cal);

    mag_bias[0]        = cal.mag_bias.X;
    mag_bias[1]        = cal.mag_bias.Y;
    mag_bias[2]        = cal.mag_bias.Z;
    mag_scale[0]       = cal.mag_scale.X;
    mag_scale[1]       = cal.mag_scale.Y;
    mag_scale[2]       = cal.mag_scale.Z;
    accel_bias[0]      = cal.accel_bias.X;
    accel_bias[1]      = cal.accel_bias.Y;
    accel_bias[2]      = cal.accel_bias.Z;
    accel_scale[0]     = cal.accel_scale.X;
    accel_scale[1]     = cal.accel_scale.Y;
    accel_scale[2]     = cal.accel_scale.Z;
    gyro_staticbias[0] = cal.gyro_bias.X;
    gyro_staticbias[1] = cal.gyro_bias.Y;
    gyro_staticbias[2] = cal.gyro_bias.Z;
    gyro_scale[0]      = cal.gyro_scale.X;
    gyro_scale[1]      = cal.gyro_scale.Y;
    gyro_scale[2]      = cal.gyro_scale.Z;


    AttitudeSettingsData attitudeSettings;
    AttitudeSettingsGet(&attitudeSettings);

    // Indicates not to expend cycles on rotation
    if (attitudeSettings.BoardRotation.Roll == 0 && attitudeSettings.BoardRotation.Pitch == 0 &&
        attitudeSettings.BoardRotation.Yaw == 0) {
        rotate = 0;
    } else {
        float rotationQuat[4];
        const float rpy[3] = { attitudeSettings.BoardRotation.Roll,
                               attitudeSettings.BoardRotation.Pitch,
                               attitudeSettings.BoardRotation.Yaw };
        RPY2Quaternion(rpy, rotationQuat);
        Quaternion2R(rotationQuat, R);
        rotate = 1;
    }
}
/**
 * @}
 * @}
 */
