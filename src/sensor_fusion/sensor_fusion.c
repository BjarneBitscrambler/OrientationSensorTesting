/*
 * Copyright (c) 2015, Freescale Semiconductor, Inc.
 * Copyright 2016-2017 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*! \file sensor_fusion.c
    \brief The sensor_fusion.c file implements the top level programming interface
*/
#include <stdio.h>

#include "sensor_fusion.h"

#include "control.h"
#include "fusion.h"
#include "hal_i2c.h"
#include "hal_timer.h"
#include "status.h"

/// Poor man's inheritance for status subsystem setStatus command
/// This function is normally invoked via the "sfg." global pointer.
void setStatus(SensorFusionGlobals *sfg, fusion_status_t status)
{
    sfg->pStatusSubsystem->set(sfg->pStatusSubsystem, status);
}

/// Poor man's inheritance for status subsystem getStatus command
/// This function is normally invoked via the "sfg." global pointer.
fusion_status_t getStatus(SensorFusionGlobals *sfg)
{
  return sfg->pStatusSubsystem->get(sfg->pStatusSubsystem);
}

/// Poor man's inheritance for status subsystem queueStatus command.
/// This function is normally involved via the "sfg." global pointer.
void queueStatus(SensorFusionGlobals *sfg, fusion_status_t status)
{
    sfg->pStatusSubsystem->queue(sfg->pStatusSubsystem, status);
}

/// Poor man's inheritance for status subsystem updateStatus command.
/// This function is normally involved via the "sfg." global pointer.
void updateStatus(SensorFusionGlobals *sfg)
{
    sfg->pStatusSubsystem->update(sfg->pStatusSubsystem);
}

void testStatus(SensorFusionGlobals *sfg)
{
    sfg->pStatusSubsystem->test(sfg->pStatusSubsystem);
}

/// utility function to insert default values in the top level structure
void initSensorFusionGlobals(SensorFusionGlobals *sfg,
                             StatusSubsystem *pStatusSubsystem,
                             ControlSubsystem *pControlSubsystem)
{
    sfg->iFlags = // all of the following defines are either 0x0000 or a 1-bit value (2, 4, 8 ...) and are defined in build.h
                F_USING_ACCEL           |
                F_USING_MAG             |
                F_USING_GYRO            |
                F_USING_PRESSURE        |
                F_USING_TEMPERATURE     |
                F_ALL_SENSORS           |       // refers to all applicable sensor types for the given physical unit
                F_1DOF_P_BASIC	        |       // 1DOF pressure (altitude) and temperature: (pressure)
                F_3DOF_G_BASIC	        |	// 3DOF accel tilt: (accel)
                F_3DOF_B_BASIC	        |	// 3DOF mag eCompass (vehicle): (mag)
                F_3DOF_Y_BASIC	        |	// 3DOF gyro integration: (gyro)
                F_6DOF_GB_BASIC	        |	// 6DOF accel and mag eCompass)
                F_6DOF_GY_KALMAN        |	// 6DOF accel and gyro (Kalman): (accel + gyro)
                F_9DOF_GBY_KALMAN	;	// 9DOF accel, mag and gyro (Kalman): (accel + mag + gyro)

    sfg->pControlSubsystem = pControlSubsystem;
    sfg->pStatusSubsystem = pStatusSubsystem;
    sfg->loopcounter = 0;                     // counter incrementing each iteration of sensor fusion (typically 25Hz)
    sfg->systick_I2C = 0;                     // systick counter to benchmark I2C reads
    sfg->systick_Spare = 0;                   // systick counter for counts spare waiting for timing interrupt
    sfg->iPerturbation = 0;                   // no perturbation to be applied
    sfg->installSensor = installSensor;       // function for installing a new sensor into the structures
    sfg->initializeFusionEngine = initializeFusionEngine;   // initializes fusion variables
    sfg->readSensors = readSensors;           // function for reading a sensor
    sfg->runFusion = runFusion;               // function for running fusion algorithms
    sfg->applyPerturbation = ApplyPerturbation; // function used for step function testing
    sfg->conditionSensorReadings = conditionSensorReadings; // function does averaging, HAL adjustments, etc.
    sfg->clearFIFOs = clearFIFOs;             // function to clear FIFO flags    sfg->applyPerturbation = ApplyPerturbation; // function used for step function testing
    sfg->setStatus = setStatus;               // function to immediately set status change
    sfg->getStatus = getStatus;               // function to report status
    sfg->queueStatus = queueStatus;           // function to queue status change
    sfg->updateStatus = updateStatus;         // function to promote queued status change
    sfg->testStatus = testStatus;             // function for unit testing the status subsystem
    sfg->pSensors = NULL;                     // pointer to linked list of physical sensors
//  put error value into whoAmI as initial value
#if F_USING_ACCEL
    sfg->Accel.iWhoAmI = 0;
#endif
#if F_USING_MAG
    sfg->Mag.iWhoAmI = 0;
#endif
#if F_USING_GYRO
    sfg->Gyro.iWhoAmI = 0;
#endif
#if F_USING_PRESSURE
    sfg->Pressure.iWhoAmI = 0;
#endif
} // end initSensorFusionGlobals()

/// installSensor is used to instantiate a physical sensor driver into the
/// sensor fusion system. It doesn't actually communicate with the sensor.
/// This function is normally invoked via the "sfg." global pointer.
int8_t installSensor(
                     SensorFusionGlobals *sfg,  ///< top level fusion structure
                     struct PhysicalSensor *pSensor,    ///< pointer to structure describing physical sensor
                     uint16_t addr,             ///< I2C address for sensor (if applicable)
                     uint16_t schedule,         ///< Sensor is read each time loop_count % schedule == 0
                     registerDeviceInfo_t *busInfo, ///< information required for bus power management
                     initializeSensor_t *initialize,    ///< pointer to sensor initialization function
                     readSensor_t *read)        ///< pointer to sensor read function
{
    if (sfg && pSensor && initialize && read)
    {
    /* was  pSensor->deviceInfo.deviceInstance = busInfo->deviceInstance;
        pSensor->deviceInfo.functionParam = busInfo->functionParam;
        pSensor->deviceInfo.idleFunction = busInfo->idleFunction;
        but these aren't used. Instead of changing structs everywhere, 
        we just set to zero. */
        pSensor->deviceInfo.deviceInstance = 0;
        pSensor->deviceInfo.functionParam = NULL;
        pSensor->deviceInfo.idleFunction = NULL;

        pSensor->initialize = initialize;       // The initialization function is responsible for putting the sensor
                                                // into the proper mode for sensor fusion.
        pSensor->read = read;                   // The read function is responsible for taking sensor readings and
                                                // loading them into the sensor fusion input structures.
        pSensor->addr = addr;                   // I2C address if applicable
        pSensor->schedule = schedule;
        // Now add the new sensor at the head of the linked list
        pSensor->next = sfg->pSensors;
        sfg->pSensors = pSensor;
        return (0);
    }
    else
    {
        return (1);
    }
} // end installSensor()

// The initializeSensors function traverses the linked list of physical sensor
// types and calls the initialization function for each one.
int8_t initializeSensors(SensorFusionGlobals *sfg)
{
    struct PhysicalSensor  *pSensor;
    int8_t          s;
    int8_t          status = 0;
    for (pSensor = sfg->pSensors; pSensor != NULL; pSensor = pSensor->next)
    {
        s = pSensor->initialize(pSensor, sfg);
        if (status == 0) status = s;            // will return 1st error flag, but try all sensors
    }
    return (status);
} // end initializeSensors()

// process<Sensor>Data routines do post processing for HAL and averaging.  They
// are called from the readSensors() function below.
#if F_USING_ACCEL
void processAccelData(SensorFusionGlobals *sfg)
{
    int32_t iSum[3];		        // channel sums
    int16_t i, j;			        // counters
    if (sfg->Accel.iFIFOExceeded > 0) {
      sfg->setStatus(sfg, SOFT_FAULT);
    }

    ApplyAccelHAL(&(sfg->Accel));     // This function is board-dependent

    // calculate the average HAL-corrected measurement
    for (j = CHX; j <= CHZ; j++) iSum[j] = 0;
    for (i = 0; i < sfg->Accel.iFIFOCount; i++)
        for (j = CHX; j <= CHZ; j++) iSum[j] += sfg->Accel.iGsFIFO[i][j];
    if (sfg->Accel.iFIFOCount > 0)
    {
        for (j = CHX; j <= CHZ; j++)
        {
            sfg->Accel.iGs[j] = (int16_t)(iSum[j] / (int32_t) sfg->Accel.iFIFOCount);
            sfg->Accel.fGs[j] = (float)sfg->Accel.iGs[j] * sfg->Accel.fgPerCount;
        }
    }

    // apply precision accelerometer calibration (offset V, inverse gain invW and rotation correction R^T)
    // to map fGs onto fGc (g), iGc (counts)
    fInvertAccelCal(&(sfg->Accel), &(sfg->AccelCal));

    // update the precision accelerometer data buffer
    fUpdateAccelBuffer(&(sfg->AccelCal),
                       &(sfg->AccelBuffer),
                       &(sfg->Accel),
                       &(sfg->pControlSubsystem->AccelCalPacketOn));
    return;
} // end processAccelData()
#endif

#if F_USING_MAG
void processMagData(SensorFusionGlobals *sfg)
{
    int32_t iSum[3];		        // channel sums
    int16_t i, j;			        // counters

    if (sfg->Mag.iFIFOExceeded > 0) {
      sfg->setStatus(sfg, SOFT_FAULT);
    }

    ApplyMagHAL(&(sfg->Mag));         // This function is board-dependent

    // calculate the average HAL-corrected measurement
    for (j = CHX; j <= CHZ; j++) iSum[j] = 0;
    for (i = 0; i < sfg->Mag.iFIFOCount; i++)
	for (j = CHX; j <= CHZ; j++) iSum[j] += sfg->Mag.iBsFIFO[i][j];
    if (sfg->Mag.iFIFOCount > 0)
    {
      for (j = CHX; j <= CHZ; j++)
      {
          sfg->Mag.iBs[j] = (int16_t)(iSum[j] / (int32_t) sfg->Mag.iFIFOCount);
          sfg->Mag.fBs[j] = (float)sfg->Mag.iBs[j] * sfg->Mag.fuTPerCount;
      }
    }

    // remove hard and soft iron terms from fBs (uT) to get calibrated data fBc (uT), iBc (counts) and
    // update magnetic buffer avoiding a write while a magnetic calibration is in progress.
    // run one iteration of the time sliced magnetic calibration
    fInvertMagCal(&(sfg->Mag), &(sfg->MagCal));
    if (!sfg->MagCal.iMagBufferReadOnly)
        iUpdateMagBuffer(&(sfg->MagBuffer), &(sfg->Mag), sfg->loopcounter);
    fRunMagCalibration(&(sfg->MagCal), &(sfg->MagBuffer), &(sfg->Mag),
                           sfg->loopcounter);

    return;
} // end processMagData()
#endif

#if F_USING_GYRO
void processGyroData(SensorFusionGlobals *sfg)
{
    int32_t iSum[3];		        // channel sums
    int16_t i, j;			        // counters
    if (sfg->Gyro.iFIFOExceeded > 0) {
      sfg->setStatus(sfg, SOFT_FAULT);
    }

    ApplyGyroHAL(&(sfg->Gyro));       // This function is board-dependent

    // calculate the average HAL-corrected measurement.  This is used for offset
    // initialization, display purposes and in the 3-axis gyro-only algorithm.
    // The Kalman filters both do the full incremental rotation integration
    // right in the filters themselves.
    for (j = CHX; j <= CHZ; j++) iSum[j] = 0;
    for (i = 0; i < sfg->Gyro.iFIFOCount; i++)
        for (j = CHX; j <= CHZ; j++)
          iSum[j] += sfg->Gyro.iYsFIFO[i][j];
    if (sfg->Gyro.iFIFOCount > 0)
    {
        for (j = CHX; j <= CHZ; j++)
        {
            sfg->Gyro.iYs[j] = (int16_t)(iSum[j] / (int32_t) sfg->Gyro.iFIFOCount);
            sfg->Gyro.fYs[j] = (float)sfg->Gyro.iYs[j] * sfg->Gyro.fDegPerSecPerCount;
        }
    }
    return;
} // end processGyroData()
#endif

/// readSensors traverses the linked list of physical sensors, calling the
/// individual read functions one by one.
/// This function is normally invoked via the "sfg." global pointer.
/// If a sensor is flagged as uninitialized, an attempt is made to initialize it.
/// If a sensor does not respond, it is marked as unintialized.
int8_t readSensors(
    SensorFusionGlobals *sfg,   ///< pointer to global sensor fusion data structure
    uint8_t read_loop_counter  ///< current loop counter (used for multirate processing)
    ) 
{
    struct PhysicalSensor  *pSensor;
    int8_t          s;
    int8_t          status = SENSOR_ERROR_NONE;

    pSensor = sfg->pSensors;

    for (pSensor = sfg->pSensors; pSensor != NULL; pSensor = pSensor->next)
    {   if (pSensor->isInitialized) {
            if ( 0 == (read_loop_counter % pSensor->schedule)) {
                //read the sensor if it is its turn (per loop_counter)
                s = pSensor->read(pSensor, sfg);
                if(s != SENSOR_ERROR_NONE) {
                    //sensor reported error, so mark it uninitialized.
                    //If it becomes reinitialized next loop, init function will set flag back to sensor type
                    pSensor->isInitialized = F_USING_NONE; 
                }
                if (status == SENSOR_ERROR_NONE) status = s; // will return 1st error flag, but try all sensors
            }
        }else {
            //sensor not initialized. Make one attempt to init it.
            //If init succeeds, next time through a sensor read will be attempted
            s = pSensor->initialize(pSensor, sfg);
            if (s != SENSOR_ERROR_NONE) {
              //note that there is still an error
              status = s;
            }
        }
    }
    if (status == SENSOR_ERROR_NONE) {
        //change (or keep) status to NORMAL on next regular status update
        sfg->queueStatus(sfg, NORMAL);
    } else {
      // flag that we have problem reading sensor, which may clear later
      sfg->setStatus(sfg, SOFT_FAULT);
    }
    return (status);
} // end readSensors()

/// conditionSensorReadings() transforms raw software FIFO readings into forms that
/// can be consumed by the sensor fusion engine.  This include sample averaging
/// and (in the case of the gyro) integrations, applying hardware abstraction layers,
/// and calibration functions.
/// This function is normally invoked via the "sfg." global pointer.
void conditionSensorReadings(SensorFusionGlobals *sfg) {
#if F_USING_ACCEL
    if (sfg->Accel.isEnabled) processAccelData(sfg);
#endif

#if F_USING_MAG
    if (sfg->Mag.isEnabled) processMagData(sfg);
#endif

#if F_USING_GYRO
    if (sfg->Gyro.isEnabled) processGyroData(sfg);
#endif
    return;
} // end conditionSensorReadings()

void zeroArray(StatusSubsystem *pStatus, void* data, uint16_t size, uint16_t numElements, uint8_t check) {
  uint16_t i;
  uint8_t *d8;
  uint16_t *d16;
  uint32_t *d32;
  switch(size) {
  case 8:
    d8 = data;
    for (i=0; i<numElements; i++) d8[i]=0;
    break;
  case 16:
    d16 = data;
    for (i=0; i<numElements; i++) d16[i]=0;
    break;
  case 32:
    d32 = data;
    for (i=0; i<numElements; i++) d32[i]=0;
    break;
  default:
    pStatus->set(pStatus, HARD_FAULT);
  }
  if (check) {
    switch(size) {
    case 8:
      d8 = data;
      for (i=0; i<numElements; i++)
        if (d8[i]!=0) pStatus->set(pStatus, HARD_FAULT);
      break;
    case 16:
      d16 = data;
      for (i=0; i<numElements; i++)
        if (d16[i]!=0) pStatus->set(pStatus, HARD_FAULT);
      break;
    case 32:
      d32 = data;
      for (i=0; i<numElements; i++)
        if (d32[i]!=0) pStatus->set(pStatus, HARD_FAULT);
      break;
    }
    return;
  }
} // end zeroArray()

/// Function to clear FIFO at the end of each fusion computation
void clearFIFOs(SensorFusionGlobals *sfg) {
  // We only clear FIFOs if the sensors are enabled.  This allows us
  // to continue to use these values when we've shut higher power consumption
  // sensors down during periods of no activity.
#if F_USING_ACCEL
    sfg->Accel.iFIFOCount=0;
    sfg->Accel.iFIFOExceeded = false;
#endif
#if F_USING_MAG
    sfg->Mag.iFIFOCount=0;
    sfg->Mag.iFIFOExceeded = false;
#endif
#if F_USING_GYRO
    sfg->Gyro.iFIFOCount=0;
    sfg->Gyro.iFIFOExceeded = false;
#endif
} // end clearFIFOs()

/// runFusion the top level call that actually runs the sensor fusion.
/// This is a utility function which manages the various defines in build.h.
/// You should feel free to drop down a level and implement only those portions
/// of fFuseSensors() that your application needs.
/// This function is normally involved via the "sfg." global pointer.
void runFusion(SensorFusionGlobals *sfg)
{
    struct SV_1DOF_P_BASIC *pSV_1DOF_P_BASIC;
    struct SV_3DOF_G_BASIC *pSV_3DOF_G_BASIC;
    struct SV_3DOF_B_BASIC *pSV_3DOF_B_BASIC;
    struct SV_3DOF_Y_BASIC *pSV_3DOF_Y_BASIC;
    struct SV_6DOF_GB_BASIC *pSV_6DOF_GB_BASIC;
    struct SV_6DOF_GY_KALMAN *pSV_6DOF_GY_KALMAN;
    struct SV_9DOF_GBY_KALMAN *pSV_9DOF_GBY_KALMAN;
    struct AccelSensor *pAccel;
    struct MagSensor *pMag;
    struct GyroSensor *pGyro;
    struct PressureSensor *pPressure;
    struct MagCalibration *pMagCal;
#if F_1DOF_P_BASIC
    pSV_1DOF_P_BASIC = &(sfg->SV_1DOF_P_BASIC);
#else
    pSV_1DOF_P_BASIC = NULL;
#endif
#if F_3DOF_G_BASIC
    pSV_3DOF_G_BASIC = &(sfg->SV_3DOF_G_BASIC)  ;
#else
    pSV_3DOF_G_BASIC = NULL;
#endif
#if F_3DOF_B_BASIC
    pSV_3DOF_B_BASIC = &(sfg->SV_3DOF_B_BASIC);
#else
    pSV_3DOF_B_BASIC = NULL;
#endif
#if F_3DOF_Y_BASIC
    pSV_3DOF_Y_BASIC = &(sfg->SV_3DOF_Y_BASIC);
#else
    pSV_3DOF_Y_BASIC = NULL;
#endif
#if F_6DOF_GB_BASIC
    pSV_6DOF_GB_BASIC = &(sfg->SV_6DOF_GB_BASIC);
#else
    pSV_6DOF_GB_BASIC = NULL;
#endif
#if F_6DOF_GY_KALMAN
    pSV_6DOF_GY_KALMAN = &(sfg->SV_6DOF_GY_KALMAN);
#else
    pSV_6DOF_GY_KALMAN = NULL;
#endif
#if F_9DOF_GBY_KALMAN
    pSV_9DOF_GBY_KALMAN = &(sfg->SV_9DOF_GBY_KALMAN);
#else
    pSV_9DOF_GBY_KALMAN = NULL;
#endif
#if F_USING_ACCEL
    pAccel =  &(sfg->Accel);
#else
    pAccel = NULL;
#endif
#if F_USING_MAG
    pMag = &(sfg->Mag);
    pMagCal = &(sfg->MagCal);
#else
    pMag = NULL;
    pMagCal = NULL;
#endif
#if F_USING_GYRO
    pGyro = &(sfg->Gyro);
#else
    pGyro = NULL;
#endif
#if F_USING_PRESSURE
    pPressure = &(sfg->Pressure);
#else
    pPressure = NULL;
#endif

    // conditionSensorReadings(sfg);  must be called prior to this function
    // fuse the sensor data
    fFuseSensors(pSV_1DOF_P_BASIC, pSV_3DOF_G_BASIC,
                 pSV_3DOF_B_BASIC, pSV_3DOF_Y_BASIC,
                 pSV_6DOF_GB_BASIC, pSV_6DOF_GY_KALMAN,
                 pSV_9DOF_GBY_KALMAN, pAccel, pMag, pGyro,
                 pPressure, pMagCal);
    clearFIFOs(sfg);
} // end runFusion()

/// This function is responsible for initializing the system prior to starting
/// the main fusion loop. I2C is initted, sensors configured, calibrations loaded.
/// This function is normally invoked via the "sfg." global pointer.
/// Fusion system status is set to:
///   INITIALIZING at the start of this function,
///   HARD_FAULT if a problem occurs initializing the I2C hardware,
///   SOFT_FAULT if a sensor doesn't initialize (it could be corrected later),
///   NORMAL when function ends, assuming no problem occurred
void initializeFusionEngine(SensorFusionGlobals *sfg, int pin_i2c_sda, int pin_i2c_scl)
{
    int16_t status = SENSOR_ERROR_NONE;
    struct ControlSubsystem    *pComm;
    pComm = sfg->pControlSubsystem;

    sfg->setStatus(sfg, INITIALIZING);
    if( ! I2CInitialize(pin_i2c_sda, pin_i2c_scl) ) {
        sfg->setStatus(sfg, HARD_FAULT);  // Never returns
    }
    status = initializeSensors(sfg);
    if (status!=SENSOR_ERROR_NONE) {  // fault condition found - will try again later
        sfg->setStatus(sfg, SOFT_FAULT);
    }

    // recall: typedef enum quaternion {Q3, Q3M, Q3G, Q6MA, Q6AG, Q9} quaternion_type;
    // Set the default quaternion to the most sophisticated supported by this build
    pComm->DefaultQuaternionPacketType = Q3;
    if (sfg->iFlags & F_3DOF_B_BASIC) pComm->DefaultQuaternionPacketType = Q3M;
    if (sfg->iFlags & F_3DOF_Y_BASIC) pComm->DefaultQuaternionPacketType = Q3G;
    if (sfg->iFlags & F_6DOF_GB_BASIC) pComm->DefaultQuaternionPacketType = Q6MA;
    if (sfg->iFlags & F_6DOF_GY_KALMAN) pComm->DefaultQuaternionPacketType = Q6AG;
    if (sfg->iFlags & F_9DOF_GBY_KALMAN) pComm->DefaultQuaternionPacketType = Q9;
    pComm->QuaternionPacketType = pComm->DefaultQuaternionPacketType ;

    // initialize the sensor fusion algorithms
    fInitializeFusion(sfg);

    // reset the loop counter to zero for first iteration
    sfg->loopcounter = 0;

    // initialize the magnetic calibration and magnetometer data buffer
#if F_USING_MAG
    fInitializeMagCalibration(&sfg->MagCal, &sfg->MagBuffer);
#endif

    // initialize the precision accelerometer calibration and accelerometer data buffer
#if F_USING_ACCEL
    fInitializeAccelCalibration(&sfg->AccelCal, &sfg->AccelBuffer, &sfg->pControlSubsystem->AccelCalPacketOn );
#endif

    clearFIFOs(sfg);

    if( status == SENSOR_ERROR_NONE ) {
        //nothing went wrong, so set status to normal
        sfg->setStatus(sfg, NORMAL);
    }

} // end initializeFusionEngine()

void conditionSample(int16_t sample[3])
{
    // This function should be called for every 16 bit sample read from sensor hardware.
    // It is responsible for making sure that we never pass on the value of -32768.
    // That value cannot be properly negated using 16-bit twos complement math.
    // The ability to be later negated is required for general compatibility
    // with possible HAL (Hardware abstraction logic) which is run later in
    // the processing pipeline.
    if (sample[CHX] == -32768) sample[CHX]++;
    if (sample[CHY] == -32768) sample[CHY]++;
    if (sample[CHZ] == -32768) sample[CHZ]++;
} // end conditionSample()

void addToFifo(union FifoSensor *sensor, uint16_t maxFifoSize, int16_t sample[3])
{
  // Note that FifoSensor is a union of GyroSensor, MagSensor and AccelSensor.
  // All contain FIFO structures in the same location.  We use the Accel
  // structure to index here.

  // example usage: if (status==SENSOR_ERROR_NONE) addToFifo((FifoSensor*) &(sfg->Mag), MAG_FIFO_SIZE, sample);
    uint8_t fifoCount = sensor->Accel.iFIFOCount;
    if (fifoCount < maxFifoSize) {
        // we have room for the new sample
        sensor->Accel.iGsFIFO[fifoCount][CHX] = sample[CHX];
        sensor->Accel.iGsFIFO[fifoCount][CHY] = sample[CHY];
        sensor->Accel.iGsFIFO[fifoCount][CHZ] = sample[CHZ];
        sensor->Accel.iFIFOCount += 1;
        sensor->Accel.iFIFOExceeded = 0;
    } else {
        //there is no room for a new sample
        sensor->Accel.iFIFOExceeded += 1;
    }
} // end addToFifo()

