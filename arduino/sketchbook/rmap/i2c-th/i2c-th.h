/**********************************************************************
Copyright (C) 2017  Marco Baldinetti <m.baldinetti@digiteco.it>
authors:
Marco Baldinetti <m.baldinetti@digiteco.it>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of
the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**********************************************************************/

#include "i2c-th-config.h"
#include <debug.h>
#include <hardware_config.h>
#include <i2c_utility.h>
#include <rmap_utility.h>
#include <sleep_utility.h>
#include <eeprom_utility.h>
#include <Wire.h>
#include <Time.h>
#include <typedef.h>
#include <registers-th.h>
#include <SensorDriver.h>

/**********************************************************************
 * TYPEDEF
 *********************************************************************/
 /*!
   \typedef
   \struct sample_t
   Sampling values for temperature and humidity
 */
 typedef struct {
   uint16_t values[SAMPLE_COUNT_MAX];
   uint8_t count;
   uint8_t error_count;
 } sample_t;

 /*!
   \typedef
   \struct observation_t
   Observations values for temperature and humidity
   med: medium values of samples
 */
 typedef struct {
   uint16_t med[OBSERVATION_COUNT];
   uint16_t count;
   uint16_t *read_ptr;
   uint16_t *write_ptr;
 } observation_t;

/*!
  \typedef
  \struct configuration_t
  EEPROM saved configuration this module.
*/
typedef struct {
  uint8_t module_type;      //!< module type saved in eeprom. If matching the MODULE_TYPE, the configuration is up to date
  uint8_t module_version;   //!< module version saved in eeprom. If matching the MODULE_VERSION, the configuration is up to date
  uint8_t i2c_address;      //!< i2c address saved in eeprom
  bool is_oneshot;          //!< oneshot modality saved in eeprom
  bool is_continuous;
  uint8_t i2c_temperature_address;
  uint8_t i2c_humidity_address;
} configuration_t;

/*!
  \typedef
  \struct readable_data_t
  Readable data through i2c bus.
*/
typedef struct {
  uint8_t module_type;      //!< module type defined in MODULE_TYPE
  uint8_t module_version;   //!< module version defined in MODULE_VERSION
  value_t temperature;
  value_t humidity;
} readable_data_t;

/*!
  \typedef
  \struct writable_data_t
  Writable data through i2c bus.
*/
typedef struct {
  uint8_t i2c_address;      //!< i2c address
  bool is_oneshot;          //!< oneshot modality saved in eeprom
  bool is_continuous;
  uint8_t i2c_temperature_address;
  uint8_t i2c_humidity_address;
} writable_data_t;

/**********************************************************************
 * ENUMERATION
 *********************************************************************/
/*!
  \enum state
  Module state machine.
*/
enum {
  INIT,
  #if (USE_POWER_DOWN)
  ENTER_POWER_DOWN,
  #endif
  TASKS_EXECUTION,
  END
} state;

typedef enum {
  SENSORS_READING_INIT,
  SENSORS_READING_PREPARE,
  SENSORS_READING_IS_PREPARED,
  SENSORS_READING_GET,
  SENSORS_READING_IS_GETTED,
  SENSORS_READING_READ,
  SENSORS_READING_NEXT,
  SENSORS_READING_END,
  SENSORS_READING_WAIT_STATE
} sensors_reading_state_t;

/**********************************************************************
 * GLOBAL VARIABLE
 *********************************************************************/
/*!
  \var configuration
  Configuration for this module.
*/
configuration_t configuration;

/*!
  \var timer_counter
  timer counter variable.
*/
volatile uint8_t timer_counter;

/*!
  \var readable_data_1
  First readable i2c register.
*/
volatile readable_data_t readable_data_1;

/*!
  \var readable_data_2
  Second readable i2c register.
*/
volatile readable_data_t readable_data_2;

/*!
  \var readable_data_read_ptr
  Pointer for read data in i2c readable register.
*/
volatile readable_data_t *readable_data_read_ptr;

/*!
  \var readable_data_write_ptr
  Pointer for write data in i2c readable register.
*/
volatile readable_data_t *readable_data_write_ptr;

/*!
  \var readable_data_temp_ptr
  Temporary pointer for exchange read and write pointer for i2c readable register.
*/
volatile readable_data_t *readable_data_temp_ptr;

/*!
  \var writable_data
  Writable i2c register.
*/
writable_data_t writable_data;

/*!
  \var writable_data
  Pointer for read and write data in i2c writable register.
*/
writable_data_t *writable_data_ptr;

/*!
  \var readable_data_address
  Address of readable i2c register.
*/
volatile uint8_t readable_data_address;

/*!
  \var readable_data_length
  Number of bytes to read at readable_data_address.
*/
volatile uint8_t readable_data_length;

/*!
  \var i2c_rx_data
  Buffer for i2c receive data.
*/
volatile uint8_t i2c_rx_data[I2C_MAX_DATA_LENGTH];

/*!
  \var i2c_rx_data_length
  Length of i2c received data.
*/
// volatile uint8_t i2c_rx_data_length;

/*!
  \var ready_tasks_count
  Number of tasks ready to execute.
*/
volatile uint8_t ready_tasks_count;

/*!
  \var awakened_event_occurred_time_ms
  System time (in millisecond) when the system has awakened from power down.
*/
uint32_t awakened_event_occurred_time_ms;

SensorDriver *sensors[USE_SENSORS_COUNT];
uint8_t sensors_count;

uint8_t sensors_end_readings_count = 0;
uint32_t absolute_millis_for_sensors_read_ms;

sample_t temperature_samples;
observation_t temperature_observations;

sample_t humidity_samples;
observation_t humidity_observations;

uint8_t samples_count;

bool is_start;
bool is_stop;
bool is_oneshot;
bool is_continuous;

bool is_first_run;

#if (USE_SENSOR_ADT)
bool is_sensor_adt_prepared;
bool is_sensor_adt_setted;
#endif

#if (USE_SENSOR_HIH)
bool is_sensor_hih_prepared;
bool is_sensor_hih_setted;
#endif

#if (USE_SENSOR_HYT)
bool is_sensor_hyt_prepared;
bool is_sensor_hyt_setted;
#endif

sensors_reading_state_t sensors_reading_state;

/**********************************************************************
 * FUNCTIONS
 *********************************************************************/
void init_wdt(uint8_t wdt_timer);
void init_systems(void);
void init_buffers(void);
void init_tasks(void);
void init_pins(void);
void init_wire(void);
void init_spi(void);
void init_rtc(void);
void init_sensors(void);

/*! \fn void print_configuration(void)
 *  \brief Print configuration.
 *  \return void.
 */
void print_configuration(void);

/*! \fn void load_configuration(void)
 *  \brief Load configuration from EEPROM.
 *  \return void.
 */
void load_configuration(void);

/*! \fn void save_configuration(bool is_default)
 *  \brief Save configuration to EEPROM.
 *  \param is_default: if true save default configuration; if false save current configuration.
 *  \return void.
 */
void save_configuration(bool);

void init_sensors(void);

void commands(void);

void start_timer(void);
void stop_timer(void);

void reset_samples_buffer(void);
void reset_observations_buffer(void);
void exchange_buffers(void);

void samples_processing(bool is_force_processing);
void observations_processing(void);
bool make_observation_from_samples(bool is_force_processing, sample_t *sample, observation_t *observation);
bool make_value_from_samples_and_observations(sample_t *sample, observation_t *observation, volatile value_t *value);

/**********************************************************************
 * TASKS
 *********************************************************************/

/*! \fn void sensors_reading_task(void)
 *  \brief Read sensors.
 *  \return void.
 */
void sensors_reading_task(void);

volatile uint32_t is_event_sensors_reading;

volatile bool is_event_command_task;

/*! \fn void command_task(void)
 *  \brief Executes the command received on i2c bus.
 *  \return void.
 */
void command_task(void);

/**********************************************************************
 * INTERRUPT HANDLER
 *********************************************************************/

/*! \fn void i2c_request_interrupt_handler(void)
 *  \brief I2C request interrupt handler.
 *  \return void.
 */
void i2c_request_interrupt_handler(void);

/*! \fn void i2c_receive_interrupt_handler(void)
 *  \brief I2C receive interrupt handler.
 *  \return void.
 */
void i2c_receive_interrupt_handler(void);
