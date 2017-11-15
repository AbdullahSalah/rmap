/**@file rmap.ino */

/*********************************************************************
Copyright (C) 2017  Marco Baldinetti <m.baldinetti@digiteco.it>
authors:
Paolo Patruno <p.patruno@iperbole.bologna.it>
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

#include <debug_config.h>

/*!
\def SERIAL_TRACE_LEVEL
\brief Serial debug level for this sketch.
*/
#define SERIAL_TRACE_LEVEL    (RMAP_SERIAL_TRACE_LEVEL)

/*!
\def LCD_TRACE_LEVEL
\brief LCD debug level for this sketch.
*/
#define LCD_TRACE_LEVEL       (RMAP_LCD_TRACE_LEVEL)

#include "rmap.h"

/*!
\fn void setup()
\brief Arduino setup function. Init watchdog, hardware, debug, buffer and load configuration stored in EEPROM.
\return void.
*/
void setup() {
   init_wdt(WDT_TIMER);
   SERIAL_BEGIN(115200);
   init_pins();
   init_wire();
   LCD_BEGIN(&lcd, LCD_COLUMNS, LCD_ROWS);
   load_configuration();
   init_buffers();
   init_spi();
   init_rtc();
   #if (USE_TIMER_1)
   init_timer1();
   #endif
   init_system();
   wdt_reset();
}

/*!
\fn void loop()
\brief Arduino loop function. First, initialize tasks and sensors, then execute the tasks and activates the power down if no task is running.
\return void.
*/
void loop() {
   switch (state) {
      case INIT:
         init_tasks();
         init_sensors();
         wdt_reset();
         state = TASKS_EXECUTION;
      break;

      #if (USE_POWER_DOWN)
      case ENTER_POWER_DOWN:
         init_power_down(&awakened_event_occurred_time_ms, DEBOUNCING_POWER_DOWN_TIME_MS);
         state = TASKS_EXECUTION;
      break;
      #endif

      case TASKS_EXECUTION:
         if (is_event_rtc) {
            rtc_task();
            wdt_reset();
         }

         if (is_event_supervisor) {
            supervisor_task();
            wdt_reset();
         }

         #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
         if (is_event_ethernet) {
            ethernet_task();
            wdt_reset();
         }

         #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
         if (is_event_gsm) {
            gsm_task();
            wdt_reset();
         }

         #endif

         if (is_event_sensors_reading) {
            sensors_reading_task();
            wdt_reset();
         }

         if (is_event_data_saving) {
            data_saving_task();
            wdt_reset();
         }

         if (is_event_mqtt) {
            mqtt_task();
            wdt_reset();
         }

         if (is_event_time) {
            time_task();
            wdt_reset();
         }

         if (ready_tasks_count == 0) {
            wdt_reset();
            state = END;
         }
      break;

      case END:
         #if (USE_POWER_DOWN)
         state = ENTER_POWER_DOWN;
         #else
         state = TASKS_EXECUTION;
         #endif
      break;
   }
}

void init_power_down(uint32_t *time_ms, uint32_t debouncing_ms) {
	if (millis() - *time_ms > debouncing_ms) {
		*time_ms = millis();

		power_adc_disable();
		power_spi_disable();
		power_timer0_disable();
      #if (USE_TIMER_1 == false)
      power_timer1_disable();
      #endif
		power_timer2_disable();

		noInterrupts ();
		sleep_enable();

		// turn off brown-out enable in software
		MCUCR = bit (BODS) | bit (BODSE);
		MCUCR = bit (BODS);
		interrupts ();

		sleep_cpu();
		sleep_disable();

		power_adc_enable();
		power_spi_enable();
		power_timer0_enable();
      #if (USE_TIMER_1 == false)
      power_timer1_enable();
      #endif
		power_timer2_enable();
	}
}

void init_buffers() {
}

void init_tasks() {
   noInterrupts();
   ready_tasks_count = 0;

   is_event_supervisor = true;
   ready_tasks_count++;

   is_event_time = false;
   is_event_sensors_reading = false;
   is_event_data_saving = false;
   is_event_mqtt = false;
   is_event_mqtt_paused = false;

   is_event_rtc = false;

   #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
   is_event_ethernet = false;
   ethernet_state = ETHERNET_INIT;

   #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
   is_event_gsm = false;
   gsm_state = GSM_INIT;

   #endif

   supervisor_state = SUPERVISOR_INIT;
   time_state = TIME_INIT;
   sensors_reading_state = SENSORS_READING_INIT;
   data_saving_state = DATA_SAVING_INIT;
   mqtt_state = MQTT_INIT;

   is_client_connected = false;
   is_client_udp_socket_open = false;

   do_ntp_sync = false;
   is_time_set = false;
   last_ntp_sync = -NTP_TIME_FOR_RESYNC_S;

   last_lcd_begin = 0;

   is_time_for_sensors_reading_updated = false;

   is_sdcard_error = false;
   is_sdcard_open = false;

   is_mqtt_subscribed = false;

   interrupts();
}

void init_pins() {
   pinMode(CONFIGURATION_RESET_PIN, INPUT_PULLUP);

   pinMode(RTC_INTERRUPT_PIN, INPUT_PULLUP);

   pinMode(SDCARD_CHIP_SELECT_PIN, OUTPUT);
   digitalWrite(SDCARD_CHIP_SELECT_PIN, HIGH);

   #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
   Ethernet.w5500_cspin = W5500_CHIP_SELECT_PIN;

   #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
   s800.init(GSM_ON_OFF_PIN);

   #endif
}

void init_wire() {
   uint8_t i2c_bus_state = I2C_ClearBus(); // clear the I2C bus first before calling Wire.begin()

   if (i2c_bus_state) {
      SERIAL_ERROR("I2C bus error: Could not clear!!!\r\n");
      while(1);
   }

   switch (i2c_bus_state) {
      case 1:
         SERIAL_ERROR("SCL clock line held low\r\n");
      break;

      case 2:
         SERIAL_ERROR("SCL clock line held low by slave clock stretch\r\n");
      break;

      case 3:
         SERIAL_ERROR("SDA data line held low\r\n");
      break;
   }

   Wire.begin();
   Wire.setClock(I2C_BUS_CLOCK);
}

void init_spi() {
   SPI.begin();
}

void init_rtc() {
   Pcf8563::disableAlarm();
   Pcf8563::disableTimer();
   Pcf8563::disableClockout();
   Pcf8563::setClockoutFrequency(RTC_FREQUENCY);
   Pcf8563::enableClockout();
   attachInterrupt(digitalPinToInterrupt(RTC_INTERRUPT_PIN), rtc_interrupt_handler, RISING);
}

void init_system() {
   #if (USE_POWER_DOWN)
   set_sleep_mode(SLEEP_MODE_PWR_DOWN);
   awakened_event_occurred_time_ms = millis();
   #endif

   // main loop state
   state = INIT;
}

void init_wdt(uint8_t wdt_timer) {
   wdt_disable();
   wdt_reset();
   wdt_enable(wdt_timer);
}

#if (USE_TIMER_1)
void init_timer1() {
}
#endif

void init_sensors () {
   is_first_run = true;
   uint8_t sensors_count = 0;
   uint8_t sensors_name_length = 20;
   char sensors_name[20];
   sensors_name[0] = 0;

   LCD_INFO(&lcd, false, "--- www.rmap.cc ---");
   LCD_INFO(&lcd, false, "%s v. %u", stima_name, readable_configuration.module_version);

   SERIAL_INFO("Sensors... [ %s ]\r\n", readable_configuration.sensors_count ? OK_STRING : ERROR_STRING);

   if (readable_configuration.sensors_count == 0) {
      LCD_INFO(&lcd, false, "Sensors not found!");
   }

   // read sensors configuration, create and setup
   for (uint8_t i=0; i<readable_configuration.sensors_count; i++) {
      SensorDriver::createAndSetup(readable_configuration.sensors[i].driver, readable_configuration.sensors[i].type, readable_configuration.sensors[i].address, sensors, &sensors_count);
      SERIAL_INFO("--> %u: %s-%s [ 0x%x ]: %s\t [ %s ]\r\n", sensors_count, readable_configuration.sensors[i].driver, readable_configuration.sensors[i].type, readable_configuration.sensors[i].address, readable_configuration.sensors[i].mqtt_topic, sensors[i]->isSetted() ? OK_STRING : FAIL_STRING);
      sensors_name_length -= snprintf(sensors_name + strlen(sensors_name), sensors_name_length, "%s ", readable_configuration.sensors[i].type);

      if (sensors_name_length < 3) {
         LCD_INFO(&lcd, false, "%s", sensors_name);
         sensors_name[0] = 0;
         sensors_name_length = 20;
      }
   }

   SERIAL_INFO("\r\n");
}

void setNextTimeForSensorReading (time_t *next_time) {
   time_t counter = (now() / readable_configuration.report_seconds);
   *next_time = (time_t) ((++counter) * readable_configuration.report_seconds);
}

bool mqttConnect(char *username, char *password) {
   char lon[10];
   char lat[10];
   char client_id[20];
   getLonLatFromMqttTopic(readable_configuration.mqtt_root_topic, lon, lat);
   snprintf(client_id, 20, "%s_%s", lon, lat);

   MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
   data.MQTTVersion = 3;
   data.clientID.cstring = (char*) client_id;
   data.username.cstring = (char*) username;
   data.password.cstring = (char*) password;
   data.cleansession = false;

   SERIAL_DEBUG("MQTT clientID: %s\r\n", data.clientID.cstring);

   return (mqtt_client.connect(data) == 0);
}

bool mqttPublish(const char *topic, const char *message) {
   MQTT::Message tx_message;
   tx_message.qos = MQTT::QOS1;
   tx_message.retained = false;
   tx_message.dup = false;
   tx_message.payload = (void*) message;
   tx_message.payloadlen = strlen(message);

   return (mqtt_client.publish(topic, tx_message) == 0);
}

void mqttRxCallback(MQTT::MessageData &md) {
   MQTT::Message &rx_message = md.message;
   SERIAL_DEBUG("%s\r\n", (char*)rx_message.payload);
   SERIAL_DEBUG("--> qos %u\r\n", rx_message.qos);
   SERIAL_DEBUG("--> retained %u\r\n", rx_message.retained);
   SERIAL_DEBUG("--> dup %u\r\n", rx_message.dup);
   SERIAL_DEBUG("--> id %u\r\n", rx_message.id);
}

void print_configuration() {
   getStimaNameByType(stima_name, readable_configuration.module_type);
   SERIAL_INFO("--> type: %s\r\n", stima_name);
   SERIAL_INFO("--> version: %d\r\n", readable_configuration.module_version);
   SERIAL_INFO("--> sensors: %d\r\n", readable_configuration.sensors_count);

   #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
   SERIAL_INFO("--> dhcp: %s\r\n", readable_configuration.is_dhcp_enable ? "on" : "off");
   SERIAL_INFO("--> ethernet mac: %02X:%02X:%02X:%02X:%02X:%02X\r\n", readable_configuration.ethernet_mac[0], readable_configuration.ethernet_mac[1], readable_configuration.ethernet_mac[2], readable_configuration.ethernet_mac[3], readable_configuration.ethernet_mac[4], readable_configuration.ethernet_mac[5]);

   #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
   SERIAL_INFO("--> gsm apn: %s\r\n", readable_configuration.gsm_apn);
   SERIAL_INFO("--> gsm username: %s\r\n", readable_configuration.gsm_username);
   SERIAL_INFO("--> gsm password: %s\r\n", readable_configuration.gsm_password);

   #endif

   SERIAL_INFO("--> ntp server: %s\r\n", readable_configuration.ntp_server);

   SERIAL_INFO("--> mqtt server: %s\r\n", readable_configuration.mqtt_server);
   SERIAL_INFO("--> mqtt port: %u\r\n", readable_configuration.mqtt_port);
   SERIAL_INFO("--> mqtt root topic: %s\r\n", readable_configuration.mqtt_root_topic);
   SERIAL_INFO("--> mqtt subscribe topic: %s\r\n", readable_configuration.mqtt_subscribe_topic);
   SERIAL_INFO("--> mqtt username: %s\r\n", readable_configuration.mqtt_username);
   SERIAL_INFO("--> mqtt password: %s\r\n\r\n", readable_configuration.mqtt_password);
}

void set_default_configuration() {
   writable_configuration.module_type = MODULE_TYPE;
   writable_configuration.module_version = MODULE_VERSION;

   writable_configuration.report_seconds = 0;

   writable_configuration.sensors_count = 0;
   memset(writable_configuration.sensors, 0, sizeof(sensor_t) * USE_SENSORS_COUNT);

   #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
   char temp_string[20];
   writable_configuration.is_dhcp_enable = CONFIGURATION_DEFAULT_ETHERNET_DHCP_ENABLE;
   strcpy(temp_string, CONFIGURATION_DEFAULT_ETHERNET_MAC);
   macStringToArray(writable_configuration.ethernet_mac, temp_string);
   strcpy(temp_string, CONFIGURATION_DEFAULT_ETHERNET_IP);
   ipStringToArray(writable_configuration.ip, temp_string);
   strcpy(temp_string, CONFIGURATION_DEFAULT_ETHERNET_NETMASK);
   ipStringToArray(writable_configuration.netmask, temp_string);
   strcpy(temp_string, CONFIGURATION_DEFAULT_ETHERNET_GATEWAY);
   ipStringToArray(writable_configuration.gateway, temp_string);
   strcpy(temp_string, CONFIGURATION_DEFAULT_ETHERNET_PRIMARY_DNS);
   ipStringToArray(writable_configuration.primary_dns, temp_string);

   #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
   strcpy(writable_configuration.gsm_apn, CONFIGURATION_DEFAULT_GSM_APN);
   strcpy(writable_configuration.gsm_username, CONFIGURATION_DEFAULT_GSM_USERNAME);
   strcpy(writable_configuration.gsm_password, CONFIGURATION_DEFAULT_GSM_PASSWORD);

   #endif

   strcpy(writable_configuration.ntp_server, CONFIGURATION_DEFAULT_NTP_SERVER);

   writable_configuration.mqtt_port = CONFIGURATION_DEFAULT_MQTT_PORT;
   strcpy(writable_configuration.mqtt_server, CONFIGURATION_DEFAULT_MQTT_SERVER);
   strcpy(writable_configuration.mqtt_root_topic, CONFIGURATION_DEFAULT_MQTT_ROOT_TOPIC);
   strcpy(writable_configuration.mqtt_subscribe_topic, CONFIGURATION_DEFAULT_MQTT_SUBSCRIBE_TOPIC);
   strcpy(writable_configuration.mqtt_username, CONFIGURATION_DEFAULT_MQTT_USERNAME);
   strcpy(writable_configuration.mqtt_password, CONFIGURATION_DEFAULT_MQTT_PASSWORD);

   SERIAL_INFO("Reset configuration to default value... [ %s ]\r\n", OK_STRING);
}

void save_configuration(bool is_default) {
   if (is_default) {
      set_default_configuration();
      SERIAL_INFO("Save default configuration... [ %s ]\r\n", OK_STRING);
   }
   else {
      SERIAL_INFO("Save configuration... [ %s ]\r\n", OK_STRING);
   }

   ee_write(&writable_configuration, CONFIGURATION_EEPROM_ADDRESS, sizeof(configuration_t));
}

void load_configuration() {
   bool is_configuration_done = false;

   ee_read(&writable_configuration, CONFIGURATION_EEPROM_ADDRESS, sizeof(configuration_t));

   if (digitalRead(CONFIGURATION_RESET_PIN) == LOW) {
      SERIAL_INFO("Wait configuration...\r\n");
      LCD_INFO(&lcd, false, "Wait configuration");
   }

   while (digitalRead(CONFIGURATION_RESET_PIN) == LOW && !is_configuration_done) {
      is_configuration_done = stream_task(&Serial, STREAM_UART_STREAM_TIMEOUT_MS, STREAM_UART_STREAM_END_TIMEOUT_MS);
      wdt_reset();
   }

   if (is_configuration_done) {
      SERIAL_INFO("Configuration received... [ %s ]\r\n", OK_STRING);
   }

   if (writable_configuration.module_type != MODULE_TYPE || writable_configuration.module_version != MODULE_VERSION) {
      save_configuration(CONFIGURATION_DEFAULT);
   }

   ee_read(&readable_configuration, CONFIGURATION_EEPROM_ADDRESS, sizeof(configuration_t));

   SERIAL_INFO("Load configuration... [ %s ]\r\n", OK_STRING);
   print_configuration();
}

char *rpc_process(char *json) {
   bool is_error = false;
   uint8_t id = 0;

   StaticJsonBuffer<STREAM_BUFFER_LENGTH> buffer;
   JsonObject &root = buffer.parseObject(json);

   if (strcmp(root["jsonrpc"], "2.0") == 0) {
      id = root["id"].as<unsigned char>();

      if (strcmp(root["method"], "configure") == 0) {
         bool is_sensor_config = false;

         for (JsonObject::iterator it = root.get<JsonObject>("params").begin(); it != root.get<JsonObject>("params").end(); ++it) {
            if (strcmp(it->key, "reset") == 0) {
               if (it->value.as<bool>() == true) {
                  set_default_configuration();
                  LCD_INFO(&lcd, false, "Reset configuration");
               }
            }
            else if (strcmp(it->key, "save") == 0) {
               if (it->value.as<bool>() == true) {
                  save_configuration(CONFIGURATION_CURRENT);
                  LCD_INFO(&lcd, false, "Save configuration");
               }
            }
            else if (strcmp(it->key, "mqttserver") == 0) {
               strncpy(writable_configuration.mqtt_server, it->value.as<char*>(), MQTT_SERVER_LENGTH);
            }
            else if (strcmp(it->key, "mqttrootpath") == 0) {
               strncpy(writable_configuration.mqtt_root_topic, it->value.as<char*>(), MQTT_ROOT_TOPIC_LENGTH);
               strncpy(writable_configuration.mqtt_subscribe_topic, it->value.as<char*>(), MQTT_SUBSCRIBE_TOPIC_LENGTH);
               uint8_t mqtt_subscribe_topic_len = strlen(writable_configuration.mqtt_subscribe_topic);
               strncpy(writable_configuration.mqtt_subscribe_topic+mqtt_subscribe_topic_len, "rx", MQTT_SUBSCRIBE_TOPIC_LENGTH-mqtt_subscribe_topic_len);
            }
            else if (strcmp(it->key, "mqttsampletime") == 0) {
               writable_configuration.report_seconds = it->value.as<unsigned int>();
            }
            else if (strcmp(it->key, "mqttuser") == 0) {
               strncpy(writable_configuration.mqtt_username, it->value.as<char*>(), MQTT_USERNAME_LENGTH);
            }
            else if (strcmp(it->key, "mqttpassword") == 0) {
               strncpy(writable_configuration.mqtt_password, it->value.as<char*>(), MQTT_PASSWORD_LENGTH);
            }
            else if (strcmp(it->key, "ntpserver") == 0) {
               strncpy(writable_configuration.ntp_server, it->value.as<char*>(), NTP_SERVER_LENGTH);
            }
            #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
            else if (strcmp(it->key, "mac") == 0) {
               for (uint8_t i=0; i<ETHERNET_MAC_LENGTH; i++) {
                  writable_configuration.ethernet_mac[i] = it->value.as<JsonArray>()[i];
               }
            }
            #endif
            else if (strcmp(it->key, "driver") == 0) {
               strncpy(writable_configuration.sensors[writable_configuration.sensors_count].driver, it->value.as<char*>(), SENSOR_DRIVER_LENGTH);
               is_sensor_config = true;
            }
            else if (strcmp(it->key, "type") == 0) {
               strncpy(writable_configuration.sensors[writable_configuration.sensors_count].type, it->value.as<char*>(), SENSOR_TYPE_LENGTH);
               is_sensor_config = true;
            }
            else if (strcmp(it->key, "address") == 0) {
               writable_configuration.sensors[writable_configuration.sensors_count].address = it->value.as<unsigned char>();
               is_sensor_config = true;
            }
            else if (strcmp(it->key, "node") == 0) {
               writable_configuration.sensors[writable_configuration.sensors_count].node = it->value.as<unsigned char>();
               is_sensor_config = true;
            }
            else if (strcmp(it->key, "mqttpath") == 0) {
               strncpy(writable_configuration.sensors[writable_configuration.sensors_count].mqtt_topic, it->value.as<char*>(), MQTT_SENSOR_TOPIC_LENGTH);
               is_sensor_config = true;
            }
         }

         if (is_sensor_config) {
            writable_configuration.sensors_count++;
         }
      }
      else if (strcmp(root["method"], "reboot") == 0) {
         init_wdt(WDTO_15MS);
      }
   }
   else {
      is_error = true;
      SERIAL_ERROR("jsonRPC v. %s it isn't supported [ %s ]\r\n", root["jsonrpc"], ERROR_STRING);
   }

   snprintf(json, STREAM_BUFFER_LENGTH, "{\"jsonrpc\":\"2.0\",\"result\":\"%s\",\"id\":%u}", is_error ? "error" : "ok", id);
   return json;
}

void rtc_interrupt_handler() {
   if (is_time_set && now() >= next_ptr_time_for_sensors_reading) {

      sensor_reading_time.Day = day(next_ptr_time_for_sensors_reading);
      sensor_reading_time.Month = month(next_ptr_time_for_sensors_reading);
      sensor_reading_time.Year = CalendarYrToTm(year(next_ptr_time_for_sensors_reading));
      sensor_reading_time.Hour = hour(next_ptr_time_for_sensors_reading);
      sensor_reading_time.Minute = minute(next_ptr_time_for_sensors_reading);
      sensor_reading_time.Second = second(next_ptr_time_for_sensors_reading);

      setNextTimeForSensorReading((time_t *) &next_ptr_time_for_sensors_reading);
      is_time_for_sensors_reading_updated = true;

      noInterrupts();
      if (!is_event_sensors_reading) {
         is_event_sensors_reading = true;
         ready_tasks_count++;
      }

      if (is_event_mqtt) {
         is_event_mqtt_paused = true;
         is_event_mqtt = false;
         ready_tasks_count--;
      }
      interrupts();
   }

   noInterrupts();
   if (!is_event_rtc) {
      is_event_rtc = true;
      ready_tasks_count++;
   }
   interrupts();
}

void supervisor_task() {
   static uint8_t retry;
   static supervisor_state_t state_after_wait;
   static uint32_t delay_ms;
   static uint32_t start_time_ms;

   static bool is_supervisor_first_run = true;
   static bool is_time_updated;

   #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
   bool *is_event_client = &is_event_ethernet;
   #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
   bool *is_event_client = &is_event_gsm;
   #endif

   switch (supervisor_state) {
      case SUPERVISOR_INIT:
         retry = 0;
         start_time_ms = 0;
         is_time_updated = false;

         if (!*is_event_client && is_client_connected) {
            is_event_client_executed = true;
         }
         else {
            is_event_time_executed = false;
         }

         if (is_event_mqtt_paused) {
            noInterrupts();
            if (!is_event_mqtt) {
               is_event_mqtt = true;
               ready_tasks_count++;
            }
            interrupts();

            supervisor_state = SUPERVISOR_END;
            SERIAL_TRACE("SUPERVISOR_INIT ---> SUPERVISOR_END\r\n");
         }
         else {
            // need ntp sync ?
            if (!do_ntp_sync && ((now() - last_ntp_sync > NTP_TIME_FOR_RESYNC_S) || !is_time_set)) {
               do_ntp_sync = true;
               SERIAL_DEBUG("Requested NTP time sync...\r\n");
            }

            start_time_ms = millis();

            supervisor_state = SUPERVISOR_CONNECTION_LEVEL_TASK;
            SERIAL_TRACE("SUPERVISOR_INIT ---> SUPERVISOR_CONNECTION_LEVEL_TASK\r\n");
         }
      break;

      case SUPERVISOR_CONNECTION_LEVEL_TASK:
         // enable hardware related tasks for connect
         noInterrupts();
         if (!*is_event_client && !is_event_client_executed && !is_client_connected) {
            *is_event_client = true;
            ready_tasks_count++;
         }
         interrupts();
         supervisor_state = SUPERVISOR_WAIT_CONNECTION_LEVEL_TASK;
         SERIAL_TRACE("SUPERVISOR_CONNECTION_LEVEL_TASK ---> SUPERVISOR_WAIT_CONNECTION_LEVEL_TASK\r\n");
      break;

      case SUPERVISOR_WAIT_CONNECTION_LEVEL_TASK:
         // success
         if (!*is_event_client && is_event_client_executed && is_client_connected) {

            // reset time task for doing ntp sync
            if (is_client_udp_socket_open && do_ntp_sync) {
               is_event_time_executed = false;
               supervisor_state = SUPERVISOR_TIME_LEVEL_TASK;
               SERIAL_TRACE("SUPERVISOR_WAIT_CONNECTION_LEVEL_TASK ---> SUPERVISOR_TIME_LEVEL_TASK\r\n");
            }
            // doing other operations...
            else {
               do_ntp_sync = false;
               supervisor_state = SUPERVISOR_MANAGE_LEVEL_TASK;
               SERIAL_TRACE("SUPERVISOR_WAIT_CONNECTION_LEVEL_TASK ---> SUPERVISOR_MANAGE_LEVEL_TASK\r\n");
            }
         }

         // error
         if (!*is_event_client && is_event_client_executed && !is_client_connected) {
            // retry
            if ((++retry < SUPERVISOR_CONNECTION_RETRY_COUNT_MAX) || (millis() - start_time_ms < SUPERVISOR_CONNECTION_TIMEOUT_MS)) {
               is_event_client_executed = false;
               supervisor_state = SUPERVISOR_CONNECTION_LEVEL_TASK;
               SERIAL_TRACE("SUPERVISOR_WAIT_CONNECTION_LEVEL_TASK ---> SUPERVISOR_CONNECTION_LEVEL_TASK\r\n");
            }
            // fail
            else {
               supervisor_state = SUPERVISOR_TIME_LEVEL_TASK;
               SERIAL_TRACE("SUPERVISOR_WAIT_CONNECTION_LEVEL_TASK ---> SUPERVISOR_TIME_LEVEL_TASK\r\n");
            }
         }
      break;

      case SUPERVISOR_TIME_LEVEL_TASK:
         // enable time task
         noInterrupts();
         if (!is_event_time && !is_event_time_executed) {
            is_event_time = true;
            ready_tasks_count++;
         }
         interrupts();

         if (!is_event_time && is_event_time_executed) {
            is_time_updated = true;

            if (is_client_connected) {
               do_ntp_sync = false;

               #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
               noInterrupts();
               if (!*is_event_client) {
                  *is_event_client = true;
                  ready_tasks_count++;
               }
               interrupts();
               #endif
            }

            supervisor_state = SUPERVISOR_MANAGE_LEVEL_TASK;
            SERIAL_TRACE("SUPERVISOR_TIME_LEVEL_TASK ---> SUPERVISOR_MANAGE_LEVEL_TASK\r\n");
         }
      break;

      case SUPERVISOR_MANAGE_LEVEL_TASK:
         if (is_time_updated) {
            SERIAL_INFO("Current date and time is: %02u/%02u/%04u %02u:%02u:%02u\r\n\r\n", day(), month(), year(), hour(), minute(), second());
         }

         if (is_supervisor_first_run && is_time_set) {
            setNextTimeForSensorReading((time_t *) &next_ptr_time_for_sensors_reading);

            SERIAL_INFO("Acquisition scheduling...\r\n");
            SERIAL_INFO("--> observations every %u minutes\r\n", OBSERVATIONS_MINUTES);

            if (readable_configuration.report_seconds >= 60) {
               uint8_t mm = readable_configuration.report_seconds / 60;
               uint8_t ss = readable_configuration.report_seconds - mm * 60;
               if (ss) {
                  SERIAL_INFO("--> report every %u minutes and %u seconds\r\n", mm, ss);
               }
               else {
                  SERIAL_INFO("--> report every %u minutes\r\n", mm);
               }
            }
            else {
               SERIAL_INFO("--> report every %u seconds\r\n", readable_configuration.report_seconds);
            }

            SERIAL_INFO("--> starting at: %02u:%02u:%02u\r\n\r\n", hour(next_ptr_time_for_sensors_reading), minute(next_ptr_time_for_sensors_reading), second(next_ptr_time_for_sensors_reading));
            LCD_INFO(&lcd, false, "start acq %02u:%02u:%02u", hour(next_ptr_time_for_sensors_reading), minute(next_ptr_time_for_sensors_reading), second(next_ptr_time_for_sensors_reading));
         }

         // reinit lcd display
         if (last_lcd_begin == 0) {
            last_lcd_begin = now();
         }
         else if ((now() - last_lcd_begin > LCD_TIME_FOR_REINITIALIZE_S)) {
            last_lcd_begin = now();
            LCD_BEGIN(&lcd, LCD_COLUMNS, LCD_ROWS);
         }

         #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
         noInterrupts();
         if (!is_event_mqtt) {
            is_event_mqtt = true;
            ready_tasks_count++;
         }
         interrupts();

         #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
         if (!*is_event_client) {
            noInterrupts();
            if (!is_event_mqtt) {
               is_event_mqtt = true;
               ready_tasks_count++;
            }
            interrupts();
         }

         #endif

         #if (SERIAL_TRACE_LEVEL >= SERIAL_TRACE_LEVEL_INFO)
         delay_ms = DEBUG_WAIT_FOR_SLEEP_MS;
         start_time_ms = millis();
         state_after_wait = SUPERVISOR_END;
         supervisor_state = SUPERVISOR_WAIT_STATE;
         #else
         supervisor_state = SUPERVISOR_END;
         #endif
         SERIAL_TRACE("SUPERVISOR_MANAGE_LEVEL_TASK ---> SUPERVISOR_END\r\n");
      break;

      case SUPERVISOR_END:
         is_supervisor_first_run = false;
         noInterrupts();
         is_event_supervisor = false;
         ready_tasks_count--;
         interrupts();

         supervisor_state = SUPERVISOR_INIT;
         SERIAL_TRACE("SUPERVISOR_END ---> SUPERVISOR_INIT\r\n");
      break;

      case SUPERVISOR_WAIT_STATE:
         if (millis() - start_time_ms > delay_ms) {
            supervisor_state = state_after_wait;
         }
      break;
   }
}

void rtc_task() {
   if (is_time_set) {
      uint32_t current_time = Pcf8563::getTime();

      if (current_time > now()) {
         setTime(Pcf8563::getTime());
      }

      if (is_time_for_sensors_reading_updated) {
         is_time_for_sensors_reading_updated = false;
         SERIAL_INFO("Next acquisition scheduled at: %02u:%02u:%02u\r\n", hour(next_ptr_time_for_sensors_reading), minute(next_ptr_time_for_sensors_reading), second(next_ptr_time_for_sensors_reading));
         LCD_INFO(&lcd, true, "next acq %02u:%02u:%02u", hour(next_ptr_time_for_sensors_reading), minute(next_ptr_time_for_sensors_reading), second(next_ptr_time_for_sensors_reading));
      }

      noInterrupts();
      is_event_rtc = false;
      ready_tasks_count--;
      interrupts();
   }
}

void time_task() {
   static uint8_t retry;
   static time_state_t state_after_wait;
   static uint32_t delay_ms;
   static uint32_t start_time_ms;
   static bool is_set_rtc_ok;
   static time_t seconds_since_1900;
   bool is_ntp_request_ok;

   switch (time_state) {
      case TIME_INIT:
         is_set_rtc_ok = true;
         seconds_since_1900 = 0;
         retry = 0;
         state_after_wait = TIME_INIT;

         if (is_client_connected) {
            time_state = TIME_SEND_ONLINE_REQUEST;
         }
         else time_state = TIME_SET_SYNC_RTC_PROVIDER;

      break;

      case TIME_SEND_ONLINE_REQUEST:
         #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
         is_ntp_request_ok = Ntp::sendRequest(&eth_udp_client, readable_configuration.ntp_server);

         #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
         is_ntp_request_ok = Ntp::sendRequest(&s800);

         #endif

         // success
         if (is_ntp_request_ok) {
            retry = 0;
            time_state = TIME_WAIT_ONLINE_RESPONSE;
         }
         // retry
         else if (++retry < NTP_RETRY_COUNT_MAX) {
            delay_ms = NTP_RETRY_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = TIME_SEND_ONLINE_REQUEST;
            time_state = TIME_WAIT_STATE;
         }
         // fail: use old rtc time
         else {
            SERIAL_ERROR("NTP request... [ %s ]\r\n", FAIL_STRING);
            time_state = TIME_SET_SYNC_RTC_PROVIDER;
         }
      break;

      case TIME_WAIT_ONLINE_RESPONSE:
         #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
         seconds_since_1900 = Ntp::getResponse(&eth_udp_client);

         #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
         seconds_since_1900 = Ntp::getResponse(&s800);

         #endif

         // success: 1483228800 seconds for 01/01/2017 00:00:00
         if (seconds_since_1900 > NTP_VALID_START_TIME_S) {
            retry = 0;
            setTime(seconds_since_1900);
            last_ntp_sync = seconds_since_1900;
            SERIAL_DEBUG("Current NTP date and time: %02u/%02u/%04u %02u:%02u:%02u\r\n", day(), month(), year(), hour(), minute(), second());
            time_state = TIME_SET_SYNC_NTP_PROVIDER;
         }
         // retry
         else if (++retry < NTP_RETRY_COUNT_MAX) {
            delay_ms = NTP_RETRY_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = TIME_WAIT_ONLINE_RESPONSE;
            time_state = TIME_WAIT_STATE;
         }
         // fail
         else {
            retry = 0;
            time_state = TIME_SET_SYNC_RTC_PROVIDER;
         }
      break;

      case TIME_SET_SYNC_NTP_PROVIDER:
         is_set_rtc_ok &= Pcf8563::disable();
         is_set_rtc_ok &= Pcf8563::setDate(day(), month(), year()-2000, weekday()-1, 0);
         is_set_rtc_ok &= Pcf8563::setTime(hour(), minute(), second());
         is_set_rtc_ok &= Pcf8563::enable();

         if (is_set_rtc_ok && now() >= seconds_since_1900) {
            retry = 0;
            time_state = TIME_SET_SYNC_RTC_PROVIDER;
         }
         // retry
         else if (++retry < NTP_RETRY_COUNT_MAX) {
            is_set_rtc_ok = true;
            delay_ms = NTP_RETRY_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = TIME_SET_SYNC_NTP_PROVIDER;
            time_state = TIME_WAIT_STATE;
         }
         // fail
         else {
            retry = 0;
            time_state = TIME_SET_SYNC_RTC_PROVIDER;
         }
      break;

      case TIME_SET_SYNC_RTC_PROVIDER:
         setSyncInterval(NTP_TIME_FOR_RESYNC_S);
         setSyncProvider(Pcf8563::getTime);
         SERIAL_DEBUG("Current RTC date and time: %02u/%02u/%04u %02u:%02u:%02u\r\n", day(), month(), year(), hour(), minute(), second());
         time_state = TIME_END;
      break;

      case TIME_END:
         is_time_set = true;
         is_event_time_executed = true;
         noInterrupts();
         is_event_time = false;
         ready_tasks_count--;
         interrupts();
         time_state = TIME_INIT;
      break;

      case TIME_WAIT_STATE:
         if (millis() - start_time_ms > delay_ms) {
            time_state = state_after_wait;
         }
      break;
   }
}

#if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
void ethernet_task() {
   static uint8_t retry;
   static ethernet_state_t state_after_wait;
   static uint32_t delay_ms;
   static uint32_t start_time_ms;

   switch (ethernet_state) {
      case ETHERNET_INIT:
         retry = 0;
         is_client_connected = false;
         is_client_udp_socket_open = false;
         state_after_wait = ETHERNET_INIT;
         ethernet_state = ETHERNET_CONNECT;
         SERIAL_TRACE("ETHERNET_INIT --> ETHERNET_CONNECT\r\n");
      break;

      case ETHERNET_CONNECT:
         if (readable_configuration.is_dhcp_enable) {
            if (Ethernet.begin(readable_configuration.ethernet_mac)) {
               is_client_connected = true;
               SERIAL_INFO("Ethernet: DHCP [ %s ]\r\n", OK_STRING);
            }
         }
         else {
            Ethernet.begin(readable_configuration.ethernet_mac, IPAddress(readable_configuration.ip), IPAddress(readable_configuration.primary_dns), IPAddress(readable_configuration.gateway), IPAddress(readable_configuration.netmask));
            is_client_connected = true;
            SERIAL_INFO("Ethernet: Static [ %s ]\r\n", OK_STRING);
         }

         // success
         if (is_client_connected) {
            w5500.setRetransmissionTime(ETHERNET_RETRY_TIME_MS);
            w5500.setRetransmissionCount(ETHERNET_RETRY_COUNT);

            SERIAL_INFO("--> ip: %u.%u.%u.%u\r\n", Ethernet.localIP()[0], Ethernet.localIP()[1], Ethernet.localIP()[2], Ethernet.localIP()[3]);
            SERIAL_INFO("--> netmask: %u.%u.%u.%u\r\n", Ethernet.subnetMask()[0], Ethernet.subnetMask()[1], Ethernet.subnetMask()[2], Ethernet.subnetMask()[3]);
            SERIAL_INFO("--> gateway: %u.%u.%u.%u\r\n", Ethernet.gatewayIP()[0], Ethernet.gatewayIP()[1], Ethernet.gatewayIP()[2], Ethernet.gatewayIP()[3]);
            SERIAL_INFO("--> primary dns: %u.%u.%u.%u\r\n", Ethernet.dnsServerIP()[0], Ethernet.dnsServerIP()[1], Ethernet.dnsServerIP()[2], Ethernet.dnsServerIP()[3]);

            LCD_INFO(&lcd, false, "ip: %u.%u.%u.%u", Ethernet.localIP()[0], Ethernet.localIP()[1], Ethernet.localIP()[2], Ethernet.localIP()[3]);

            ethernet_state = ETHERNET_OPEN_UDP_SOCKET;
            SERIAL_TRACE("ETHERNET_CONNECT --> ETHERNET_OPEN_UDP_SOCKET\r\n");
         }
         // retry
         else if ((++retry) < ETHERNET_RETRY_COUNT_MAX) {
            delay_ms = ETHERNET_RETRY_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = ETHERNET_CONNECT;
            ethernet_state = ETHERNET_WAIT_STATE;
            SERIAL_TRACE("ETHERNET_CONNECT --> ETHERNET_WAIT_STATE\r\n");
         }
         // fail
         else {
            retry = 0;
            ethernet_state = ETHERNET_END;
            SERIAL_TRACE("ETHERNET_CONNECT --> ETHERNET_END\r\n");
            SERIAL_ERROR("Ethernet %s: [ %s ]\r\n", ERROR_STRING, readable_configuration.is_dhcp_enable ? "DHCP" : "Static");
            LCD_INFO(&lcd, false, "ethernet %s", ERROR_STRING);
         }
      break;

      case ETHERNET_OPEN_UDP_SOCKET:
         // success
         if (eth_udp_client.begin(ETHERNET_DEFAULT_LOCAL_UDP_PORT)) {
            SERIAL_TRACE("--> udp socket local port: %u [ OK ]\r\n", ETHERNET_DEFAULT_LOCAL_UDP_PORT);
            is_client_udp_socket_open = true;
            ethernet_state = ETHERNET_END;
            SERIAL_TRACE("ETHERNET_OPEN_UDP_SOCKET --> ETHERNET_END\r\n");
         }
         // retry
         else if ((++retry) < ETHERNET_RETRY_COUNT_MAX) {
            delay_ms = ETHERNET_RETRY_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = ETHERNET_OPEN_UDP_SOCKET;
            ethernet_state = ETHERNET_WAIT_STATE;
            SERIAL_TRACE("ETHERNET_OPEN_UDP_SOCKET --> ETHERNET_WAIT_STATE\r\n");
         }
         // fail
         else {
            SERIAL_ERROR("--> udp socket on local port: %u [ FAIL ]\r\n", ETHERNET_DEFAULT_LOCAL_UDP_PORT);
            retry = 0;
            ethernet_state = ETHERNET_INIT;
            SERIAL_TRACE("ETHERNET_OPEN_UDP_SOCKET --> ETHERNET_INIT\r\n");
         }
      break;

      case ETHERNET_END:
         SERIAL_INFO("\r\n");
         is_event_client_executed = true;
         noInterrupts();
         is_event_ethernet = false;
         ready_tasks_count--;
         interrupts();
         ethernet_state = ETHERNET_INIT;
         SERIAL_TRACE("ETHERNET_END --> ETHERNET_INIT\r\n");
      break;

      case ETHERNET_WAIT_STATE:
         if (millis() - start_time_ms > delay_ms) {
            ethernet_state = state_after_wait;
         }
      break;
   }
}

#elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
void gsm_task() {
   static gsm_state_t state_after_wait;
   static uint32_t delay_ms;
   static uint32_t start_time_ms;
   static bool is_error;
   sim800_status_t sim800_status;
   uint8_t sim800_connection_status;
   static uint8_t power_off_mode = SIM800_POWER_OFF_BY_SWITCH;

   switch (gsm_state) {
      case GSM_INIT:
         is_error = false;
         is_client_connected = false;
         sim800_connection_status = 0;
         state_after_wait = GSM_INIT;
         gsm_state = GSM_SWITCH_ON;
      break;

      case GSM_SWITCH_ON:
         sim800_status = s800.switchOn();

         // success
         if (sim800_status == SIM800_OK) {
            gsm_state = GSM_AUTOBAUD;
         }
         else if (sim800_status == SIM800_ERROR) {
            gsm_state = GSM_END;
         }
         // wait...
      break;

      case GSM_AUTOBAUD:
         sim800_status = s800.initAutobaud();

         // success
         if (sim800_status == SIM800_OK) {
            delay_ms = SIM800_WAIT_FOR_AUTOBAUD_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = GSM_SETUP;
            gsm_state = GSM_WAIT_STATE;

         }
         // fail
         else if (sim800_status == SIM800_ERROR) {
            gsm_state = GSM_WAIT_FOR_SWITCH_OFF;
         }
         // wait...
      break;

      case GSM_SETUP:
         sim800_status = s800.setup();

         // success
         if (sim800_status == SIM800_OK) {
            gsm_state = GSM_START_CONNECTION;
         }
         // fail
         else if (sim800_status == SIM800_ERROR) {
            is_error = true;
            gsm_state = GSM_WAIT_FOR_SWITCH_OFF;
         }
         // wait...
      break;

      case GSM_START_CONNECTION:
         sim800_status = s800.startConnection(readable_configuration.gsm_apn, readable_configuration.gsm_username, readable_configuration.gsm_password);

         // success
         if (sim800_status == SIM800_OK) {
            gsm_state = GSM_CHECK_OPERATION;
         }
         // fail
         else if (sim800_status == SIM800_ERROR) {
            is_error = true;
            gsm_state = GSM_WAIT_FOR_SWITCH_OFF;
         }
         // wait...
      break;

      case GSM_CHECK_OPERATION:
         // open udp socket for query NTP
         if (do_ntp_sync) {
            gsm_state = GSM_OPEN_UDP_SOCKET;
         }
         // wait for mqtt send terminate
         else {
            gsm_state = GSM_SUSPEND;
            state_after_wait = GSM_STOP_CONNECTION;
         }
      break;

      case GSM_OPEN_UDP_SOCKET:
         sim800_connection_status = s800.connection(SIM800_CONNECTION_UDP, readable_configuration.ntp_server, NTP_SERVER_PORT);

         // success
         if (sim800_connection_status == 1) {
            is_client_udp_socket_open = true;
            is_client_connected = true;
            is_event_client_executed = true;
            state_after_wait = GSM_STOP_CONNECTION;
            gsm_state = GSM_SUSPEND;
         }
         // fail
         else if (sim800_connection_status == 2) {
            is_client_connected = false;
            is_event_client_executed = true;
            is_error = true;
            gsm_state = GSM_WAIT_FOR_SWITCH_OFF;
         }
         // wait
      break;

      case GSM_SUSPEND:
         is_client_connected = true;
         is_event_client_executed = true;
         gsm_state = state_after_wait;
         noInterrupts();
         is_event_gsm = false;
         ready_tasks_count--;
         interrupts();
      break;

      case GSM_STOP_CONNECTION:
         sim800_status = s800.stopConnection();

         // success
         if (sim800_status == SIM800_OK) {
            gsm_state = GSM_SWITCH_OFF;
         }
         // fail
         else if (sim800_status == SIM800_ERROR) {
            is_error = true;
            gsm_state = GSM_SWITCH_OFF;
         }
         // wait
      break;

      case GSM_WAIT_FOR_SWITCH_OFF:
         delay_ms = SIM800_POWER_ON_TO_OFF_DELAY_MS;
         start_time_ms = millis();
         state_after_wait = GSM_SWITCH_OFF;
         gsm_state = GSM_WAIT_STATE;
      break;

      case GSM_SWITCH_OFF:
         sim800_status = s800.switchOff(power_off_mode);

         // success
         if (sim800_status == SIM800_OK) {
            delay_ms = SIM800_WAIT_FOR_POWER_OFF_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = GSM_END;
            gsm_state = GSM_WAIT_STATE;
         }
         // fail
         else if (sim800_status == SIM800_ERROR) {
            if (power_off_mode == SIM800_POWER_OFF_BY_AT_COMMAND) {
               power_off_mode = SIM800_POWER_OFF_BY_SWITCH;
            }
            else {
               gsm_state = GSM_END;
            }
         }
         // wait...
      break;

      case GSM_END:
         if (is_error) {
         }
         is_event_client_executed = true;
         is_client_connected = false;
         is_client_udp_socket_open = false;
         noInterrupts();
         is_event_gsm = false;
         ready_tasks_count--;
         interrupts();
         gsm_state = GSM_INIT;
      break;

      case GSM_WAIT_STATE:
         if (millis() - start_time_ms > delay_ms) {
            gsm_state = state_after_wait;
         }
      break;
   }
}

#endif

void sensors_reading_task () {
   static uint8_t i;
   static uint8_t retry;
   static sensors_reading_state_t state_after_wait;
   static uint32_t delay_ms;
   static uint32_t start_time_ms;
   static int32_t values_readed_from_sensor[VALUES_TO_READ_FROM_SENSOR_COUNT];

   switch (sensors_reading_state) {
      case SENSORS_READING_INIT:
         SERIAL_INFO("Sensors reading...\r\n");

         for (i=0; i<readable_configuration.sensors_count; i++) {
            sensors[i]->resetPrepared();
         }

         i = 0;
         retry = 0;
         state_after_wait = SENSORS_READING_INIT;
         sensors_reading_state = SENSORS_READING_PREPARE;
         SERIAL_TRACE("SENSORS_READING_INIT ---> SENSORS_READING_PREPARE\r\n");
      break;

      case SENSORS_READING_PREPARE:
         sensors[i]->prepare();
         delay_ms = sensors[i]->getDelay();
         start_time_ms = sensors[i]->getStartTime();

         if (delay_ms) {
            state_after_wait = SENSORS_READING_IS_PREPARED;
            sensors_reading_state = SENSORS_READING_WAIT_STATE;
            SERIAL_TRACE("SENSORS_READING_PREPARE ---> SENSORS_READING_WAIT_STATE\r\n");
         }
         else {
            sensors_reading_state = SENSORS_READING_IS_PREPARED;
            SERIAL_TRACE("SENSORS_READING_PREPARE ---> SENSORS_READING_IS_PREPARED\r\n");
         }
      break;

      case SENSORS_READING_IS_PREPARED:
         // success
         if (sensors[i]->isPrepared()) {
            retry = 0;
            sensors_reading_state = SENSORS_READING_GET;
            SERIAL_TRACE("SENSORS_READING_IS_PREPARED ---> SENSORS_READING_GET\r\n");
         }
         // retry
         else if ((++retry) < SENSORS_RETRY_COUNT_MAX) {
            delay_ms = SENSORS_RETRY_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = SENSORS_READING_PREPARE;
            sensors_reading_state = SENSORS_READING_WAIT_STATE;
            SERIAL_TRACE("SENSORS_READING_IS_PREPARED ---> SENSORS_READING_WAIT_STATE\r\n");
         }
         // fail
         else {
            retry = 0;
            sensors_reading_state = SENSORS_READING_GET;
            SERIAL_TRACE("SENSORS_READING_IS_PREPARED ---> SENSORS_READING_GET\r\n");
         }
      break;

      case SENSORS_READING_GET:
         sensors[i]->getJson(values_readed_from_sensor, VALUES_TO_READ_FROM_SENSOR_COUNT, &json_sensors_data[i][0]);
         delay_ms = sensors[i]->getDelay();
         start_time_ms = sensors[i]->getStartTime();

         if (delay_ms) {
            state_after_wait = SENSORS_READING_IS_GETTED;
            sensors_reading_state = SENSORS_READING_WAIT_STATE;
            SERIAL_TRACE("SENSORS_READING_GET ---> SENSORS_READING_WAIT_STATE\r\n");
         }
         else {
            sensors_reading_state = SENSORS_READING_IS_GETTED;
            SERIAL_TRACE("SENSORS_READING_GET ---> SENSORS_READING_IS_GETTED\r\n");
         }
      break;

      case SENSORS_READING_IS_GETTED:
         // end
         if (sensors[i]->isEnd() && !sensors[i]->isReaded()) {
            // success
            if (sensors[i]->isSuccess()) {
               retry = 0;
               sensors_reading_state = SENSORS_READING_READ;
               SERIAL_TRACE("SENSORS_READING_IS_GETTED ---> SENSORS_READING_READ\r\n");
            }
            // retry
            else if ((++retry) < SENSORS_RETRY_COUNT_MAX) {
               delay_ms = SENSORS_RETRY_DELAY_MS;
               start_time_ms = millis();
               state_after_wait = SENSORS_READING_GET;
               sensors_reading_state = SENSORS_READING_WAIT_STATE;
               SERIAL_TRACE("SENSORS_READING_IS_GETTED ---> SENSORS_READING_WAIT_STATE\r\n");
            }
            // fail
            else {
               retry = 0;
               sensors_reading_state = SENSORS_READING_READ;
               SERIAL_TRACE("SENSORS_READING_IS_GETTED ---> SENSORS_READING_READ\r\n");
            }
         }
         // not end
         else {
            sensors_reading_state = SENSORS_READING_GET;
            SERIAL_TRACE("SENSORS_READING_IS_GETTED ---> SENSORS_READING_GET\r\n");
         }
      break;

      case SENSORS_READING_READ:
         sensors_reading_state = SENSORS_READING_NEXT;
         SERIAL_TRACE("SENSORS_READING_READ ---> SENSORS_READING_NEXT\r\n");
      break;

      case SENSORS_READING_NEXT:
         // next sensor
         if ((++i) < readable_configuration.sensors_count) {
            retry = 0;
            sensors_reading_state = SENSORS_READING_PREPARE;
            SERIAL_TRACE("SENSORS_READING_NEXT ---> SENSORS_READING_PREPARE\r\n");
         }
         // success: all sensors readed
         else {
            // first time: read ptr data from sdcard
            if (is_first_run) {
               is_first_run = false;

               noInterrupts();
               if (!is_event_supervisor && is_event_mqtt_paused) {
                  is_event_supervisor = true;
                  ready_tasks_count++;
               }
               interrupts();
            }
            // other time: save data to sdcard
            else {
               noInterrupts();
               if (!is_event_data_saving) {
                  is_event_data_saving = true;
                  ready_tasks_count++;
               }
               interrupts();
            }

            sensors_reading_state = SENSORS_READING_END;
            SERIAL_TRACE("SENSORS_READING_NEXT ---> SENSORS_READING_END\r\n");
         }
      break;

      case SENSORS_READING_END:
         noInterrupts();
         is_event_sensors_reading = false;
         ready_tasks_count--;
         interrupts();
         sensors_reading_state = SENSORS_READING_INIT;
         SERIAL_TRACE("SENSORS_READING_END ---> SENSORS_READING_INIT\r\n");
      break;

      case SENSORS_READING_WAIT_STATE:
         if (millis() - start_time_ms > delay_ms) {
            sensors_reading_state = state_after_wait;
         }
      break;
   }
}

void data_saving_task() {
   static uint8_t retry;
   static data_saving_state_t state_after_wait;
   static uint32_t delay_ms;
   static uint32_t start_time_ms;
   static uint8_t i;
   static uint8_t k;
   static uint8_t data_count;
   static uint16_t sd_data_count;
   static char sd_buffer[MQTT_SENSOR_TOPIC_LENGTH + MQTT_MESSAGE_LENGTH];
   static char topic_buffer[VALUES_TO_READ_FROM_SENSOR_COUNT][MQTT_SENSOR_TOPIC_LENGTH];
   static char message_buffer[VALUES_TO_READ_FROM_SENSOR_COUNT][MQTT_MESSAGE_LENGTH];
   char file_name[SDCARD_FILES_NAME_MAX_LENGTH];

   switch (data_saving_state) {
      case DATA_SAVING_INIT:
         retry = 0;
         sd_data_count = 0;

         if (is_sdcard_open) {
            data_saving_state = DATA_SAVING_OPEN_FILE;
            SERIAL_TRACE("DATA_SAVING_INIT ---> DATA_SAVING_OPEN_FILE\r\n");
         }
         else {
            data_saving_state = DATA_SAVING_OPEN_SDCARD;
            SERIAL_TRACE("DATA_SAVING_INIT ---> DATA_SAVING_OPEN_SDCARD\r\n");
         }
      break;

      case DATA_SAVING_OPEN_SDCARD:
         if (sdcard_init(&SD, SDCARD_CHIP_SELECT_PIN)) {
            retry = 0;
            is_sdcard_open = true;
            data_saving_state = DATA_SAVING_OPEN_FILE;
            SERIAL_TRACE("DATA_SAVING_OPEN_SDCARD ---> DATA_SAVING_OPEN_FILE\r\n");
         }
         // retry
         else if ((++retry) < DATA_SAVING_RETRY_COUNT_MAX) {
            delay_ms = DATA_SAVING_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = DATA_SAVING_OPEN_SDCARD;
            data_saving_state = DATA_SAVING_WAIT_STATE;
            SERIAL_TRACE("DATA_SAVING_OPEN_SDCARD ---> DATA_SAVING_WAIT_STATE\r\n");
         }
         // fail
         else {
            is_sdcard_error = true;
            is_sdcard_open = false;
            SERIAL_ERROR("SD Card... [ FAIL ]\r\n--> is card inserted?\r\n--> there is a valid FAT32 filesystem?\r\n\r\n");

            data_saving_state = DATA_SAVING_END;
            SERIAL_TRACE("DATA_SAVING_OPEN_SDCARD ---> DATA_SAVING_END\r\n");
         }
      break;

      case DATA_SAVING_OPEN_FILE:
         // open sdcard file: today!
         sdcard_make_filename(now(), file_name);

         // try to open file. if ok, write sensors data on it.
         if (sdcard_open_file(&SD, &write_data_file, file_name, O_RDWR | O_CREAT | O_APPEND)) {
            retry = 0;
            i = 0;
            data_saving_state = DATA_SAVING_SENSORS_LOOP;
            SERIAL_TRACE("DATA_SAVING_OPEN_FILE ---> DATA_SAVING_SENSORS_LOOP\r\n");
         }
         // retry
         else if ((++retry) < DATA_SAVING_RETRY_COUNT_MAX) {
            delay_ms = DATA_SAVING_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = DATA_SAVING_OPEN_FILE;
            data_saving_state = DATA_SAVING_WAIT_STATE;
            SERIAL_TRACE("DATA_SAVING_OPEN_SDCARD ---> DATA_SAVING_WAIT_STATE\r\n");
         }
         // fail
         else {
            SERIAL_ERROR("SD Card open file %s... [ FAIL ]\r\n", file_name);
            is_sdcard_error = true;
            data_saving_state = DATA_SAVING_END;
            SERIAL_TRACE("DATA_SAVING_OPEN_FILE ---> DATA_SAVING_END\r\n");
         }
      break;

      case DATA_SAVING_SENSORS_LOOP:
         if (i < readable_configuration.sensors_count) {
            k = 0;
            data_count = jsonToMqtt(&json_sensors_data[i][0], readable_configuration.sensors[i].mqtt_topic, topic_buffer, message_buffer, (tmElements_t *) &sensor_reading_time);
            data_saving_state = DATA_SAVING_DATA_LOOP;
            SERIAL_TRACE("DATA_SAVING_SENSORS_LOOP ---> DATA_SAVING_DATA_LOOP\r\n");
         }
         else {
            SERIAL_DEBUG("\r\n");
            data_saving_state = DATA_SAVING_CLOSE_FILE;
            SERIAL_TRACE("DATA_SAVING_SENSORS_LOOP ---> DATA_SAVING_CLOSE_FILE\r\n");
         }
      break;

      case DATA_SAVING_DATA_LOOP:
         if (k < data_count) {
            mqttToSd(&topic_buffer[k][0], &message_buffer[k][0], sd_buffer);
            data_saving_state = DATA_SAVING_WRITE_FILE;
            SERIAL_TRACE("DATA_SAVING_DATA_LOOP ---> DATA_SAVING_WRITE_FILE\r\n");
         }
         else {
            i++;
            data_saving_state = DATA_SAVING_SENSORS_LOOP;
            SERIAL_TRACE("DATA_SAVING_DATA_LOOP ---> DATA_SAVING_SENSORS_LOOP\r\n");
         }
      break;

      case DATA_SAVING_WRITE_FILE:
         // sdcard success
         if (write_data_file.write(sd_buffer, MQTT_SENSOR_TOPIC_LENGTH + MQTT_MESSAGE_LENGTH) == (MQTT_SENSOR_TOPIC_LENGTH + MQTT_MESSAGE_LENGTH)) {
            SERIAL_DEBUG("SD <-- %s %s\r\n", &topic_buffer[k][0], &message_buffer[k][0]);
            write_data_file.flush();
            retry = 0;
            k++;
            sd_data_count++;
            data_saving_state = DATA_SAVING_DATA_LOOP;
            SERIAL_TRACE("DATA_SAVING_WRITE_FILE ---> DATA_SAVING_DATA_LOOP\r\n");
         }
         // retry
         else if ((++retry) < DATA_SAVING_RETRY_COUNT_MAX) {
            delay_ms = DATA_SAVING_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = DATA_SAVING_WRITE_FILE;
            data_saving_state = DATA_SAVING_WAIT_STATE;
            SERIAL_TRACE("DATA_SAVING_WRITE_FILE ---> DATA_SAVING_WAIT_STATE\r\n");
         }
         // fail
         else {
            SERIAL_ERROR("SD Card writing data on file %s... [ FAIL ]\r\n", file_name);
            is_sdcard_error = true;
            data_saving_state = DATA_SAVING_CLOSE_FILE;
            SERIAL_TRACE("DATA_SAVING_WRITE_FILE ---> DATA_SAVING_CLOSE_FILE\r\n");
         }
      break;

      case DATA_SAVING_CLOSE_FILE:
            is_sdcard_error = !write_data_file.close();
            data_saving_state = DATA_SAVING_END;
            SERIAL_TRACE("DATA_SAVING_CLOSE_FILE ---> DATA_SAVING_END\r\n");
         break;

      case DATA_SAVING_END:
         SERIAL_INFO("[ %u ] data stored in sdcard... [ %s ]\r\n", sd_data_count, is_sdcard_error ? ERROR_STRING : OK_STRING);
         LCD_INFO(&lcd, false, "sdcard %u data %s", sd_data_count, is_sdcard_error ? ERROR_STRING : OK_STRING);

         noInterrupts();
         if (!is_event_supervisor) {
            is_event_supervisor = true;
            ready_tasks_count++;
         }

         is_event_data_saving = false;
         ready_tasks_count--;
         interrupts();

         data_saving_state = DATA_SAVING_INIT;
         SERIAL_TRACE("DATA_SAVING_END ---> DATA_SAVING_INIT\r\n");
      break;

      case DATA_SAVING_WAIT_STATE:
         if (millis() - start_time_ms > delay_ms) {
            data_saving_state = state_after_wait;
         }
      break;
   }
}

void mqtt_task() {
   static uint8_t retry;
   static mqtt_state_t state_after_wait;
   static uint32_t delay_ms;
   static uint32_t start_time_ms;
   static uint8_t i;
   static uint8_t k;
   static uint16_t mqtt_data_count;
   static uint8_t data_count;
   static char sd_buffer[MQTT_SENSOR_TOPIC_LENGTH + MQTT_MESSAGE_LENGTH];
   static char topic_buffer[VALUES_TO_READ_FROM_SENSOR_COUNT][MQTT_SENSOR_TOPIC_LENGTH];
   static char message_buffer[VALUES_TO_READ_FROM_SENSOR_COUNT][MQTT_MESSAGE_LENGTH];
   static char full_topic_buffer[MQTT_ROOT_TOPIC_LENGTH + MQTT_SENSOR_TOPIC_LENGTH];
   static bool is_mqtt_error;
   static bool is_mqtt_processing_sdcard;
   static bool is_mqtt_processing_json;
   static bool is_mqtt_published_data;
   static bool is_ptr_found;
   static bool is_ptr_updated;
   static bool is_eof_data_read;
   static tmElements_t datetime;
   static time_t current_ptr_time_data;
   static time_t last_correct_ptr_time_data;
   static time_t next_ptr_time_data;
   static uint32_t ipstack_timeout_ms;
   uint8_t ipstack_status;
   char file_name[SDCARD_FILES_NAME_MAX_LENGTH];
   int read_bytes_count;

   switch (mqtt_state) {
      case MQTT_INIT:
         retry = 0;
         is_ptr_found = false;
         is_ptr_updated = false;
         is_eof_data_read = false;
         is_mqtt_error = false;
         is_mqtt_published_data = false;
         mqtt_data_count = 0;

         if (!is_sdcard_open && !is_sdcard_error) {
            mqtt_state = MQTT_OPEN_SDCARD;
            SERIAL_TRACE("MQTT_PTR_DATA_INIT ---> MQTT_OPEN_SDCARD\r\n");
         }
         else if (is_sdcard_open) {
            mqtt_state = MQTT_OPEN_PTR_FILE;
            SERIAL_TRACE("MQTT_PTR_DATA_INIT ---> MQTT_OPEN_PTR_FILE\r\n");
         }
         else {
            mqtt_state = MQTT_PTR_END;
            SERIAL_TRACE("MQTT_PTR_DATA_INIT ---> MQTT_PTR_END\r\n");
         }
      break;

      case MQTT_OPEN_SDCARD:
         if (sdcard_init(&SD, SDCARD_CHIP_SELECT_PIN)) {
            retry = 0;
            is_sdcard_open = true;
            is_sdcard_error = false;
            mqtt_state = MQTT_OPEN_PTR_FILE;
            SERIAL_TRACE("MQTT_OPEN_SDCARD ---> MQTT_OPEN_PTR_FILE\r\n");
         }
         // retry
         else if ((++retry) < MQTT_RETRY_COUNT_MAX) {
            delay_ms = MQTT_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = MQTT_OPEN_SDCARD;
            mqtt_state = MQTT_WAIT_STATE;
            SERIAL_TRACE("MQTT_OPEN_SDCARD ---> MQTT_PTR_DATA_WAIT_STATE\r\n");
         }
         // fail
         else {
            is_sdcard_error = true;
            is_sdcard_open = false;
            SERIAL_ERROR("SD Card... [ FAIL ]\r\n--> is card inserted?\r\n--> there is a valid FAT32 filesystem?\r\n\r\n");

            mqtt_state = MQTT_PTR_END;
            SERIAL_TRACE("MQTT_OPEN_SDCARD ---> MQTT_PTR_END\r\n");
         }
         break;

      case MQTT_OPEN_PTR_FILE:
         // try to open file. if ok, read ptr data.
         if (sdcard_open_file(&SD, &mqtt_ptr_file, SDCARD_MQTT_PTR_FILE_NAME, O_RDWR | O_CREAT)) {
            retry = 0;
            mqtt_state = MQTT_PTR_READ;
            SERIAL_TRACE("MQTT_OPEN_PTR_FILE ---> MQTT_PTR_READ\r\n");
         }
         // retry
         else if ((++retry) < MQTT_RETRY_COUNT_MAX) {
            delay_ms = MQTT_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = MQTT_OPEN_PTR_FILE;
            mqtt_state = MQTT_WAIT_STATE;
            SERIAL_TRACE("MQTT_OPEN_PTR_FILE ---> MQTT_PTR_DATA_WAIT_STATE\r\n");
         }
         // fail
         else {
            SERIAL_ERROR("SD Card open file %s... [ FAIL ]\r\n", SDCARD_MQTT_PTR_FILE_NAME);
            is_sdcard_error = true;
            mqtt_state = MQTT_PTR_END;
            SERIAL_TRACE("MQTT_OPEN_PTR_FILE ---> MQTT_PTR_END\r\n");
         }
      break;

      case MQTT_PTR_READ:
         ptr_time_data = UINT32_MAX;
         mqtt_ptr_file.seekSet(0);
         read_bytes_count = mqtt_ptr_file.read(&ptr_time_data, sizeof(time_t));

         // found
         if (read_bytes_count == sizeof(time_t) && ptr_time_data < now()) {
            is_ptr_found = true;
            mqtt_state = MQTT_PTR_FOUND;
            SERIAL_TRACE("MQTT_PTR_READ ---> MQTT_PTR_FOUND\r\n");
         }
         // not found (no sdcard error): find it by starting from 1th January of this year
         else if (read_bytes_count >= 0) {
            SERIAL_INFO("Data pointer... [ FIND ]\r\n");
            datetime.Year = CalendarYrToTm(year(now()));
            datetime.Month = 1;
            datetime.Day = 1;
            datetime.Hour = 0;
            datetime.Minute = 0;
            datetime.Second = 0;
            ptr_time_data = makeTime(datetime);
            is_ptr_found = false;
            mqtt_state = MQTT_PTR_FIND;
            SERIAL_TRACE("MQTT_PTR_READ ---> MQTT_PTR_FIND\r\n");
         }
         // not found (sdcard error)
         else {
            is_ptr_found = false;
            is_sdcard_error = true;
            mqtt_state = MQTT_PTR_END;
            SERIAL_TRACE("MQTT_PTR_READ ---> MQTT_PTR_END\r\n");
         }
      break;

      case MQTT_PTR_FIND:
         // ptr not found. find it by searching in file name until today is reach.
         // if there isn't file, ptr_time_data is set to current date at 00:00:00 time.
         if (!is_ptr_found && ptr_time_data < now()) {
            sdcard_make_filename(ptr_time_data, file_name);

            if (SD.exists(file_name)) {
               is_ptr_found = true;
               is_ptr_updated = true;
               is_eof_data_read = false;
               SERIAL_INFO("%s... [ FOUND ]\r\n", file_name);
               mqtt_state = MQTT_PTR_END;
               SERIAL_TRACE("MQTT_PTR_FOUND ---> MQTT_PTR_END\r\n");
            }
            else {
               SERIAL_INFO("%s... [ NOT FOUND ]\r\n", file_name);
               ptr_time_data += SECS_PER_DAY;
            }
         }
         // ptr not found: set ptr to yesterday at 23:60-(uint8_t)(readable_configuration.report_seconds / 60):00 time.
         else if (!is_ptr_found && ptr_time_data >= now()) {
            datetime.Year = CalendarYrToTm(year());
            datetime.Month = month();
            datetime.Day = day();
            datetime.Hour = 0;
            datetime.Minute = 0;
            datetime.Second = 0;
            ptr_time_data = makeTime(datetime);
            is_ptr_found = true;
            is_ptr_updated = true;
         }
         // ptr found: sooner or later the ptr will be set in any case
         else if (is_ptr_found) {
            mqtt_state = MQTT_PTR_FOUND;
            SERIAL_TRACE("MQTT_PTR_FIND ---> MQTT_PTR_FOUND\r\n");
         }
      break;

      case MQTT_PTR_FOUND:
         // datafile read, reach eof and is today. END.
         if (is_eof_data_read && year() == year(ptr_time_data) && month() == month(ptr_time_data) && day() == day(ptr_time_data)) {
            mqtt_state = MQTT_CLOSE_DATA_FILE;
            SERIAL_TRACE("MQTT_PTR_FOUND ---> MQTT_CLOSE_DATA_FILE\r\n");
         }
         // datafile read, reach eof and NOT is today. go to end of this day.
         else if (is_eof_data_read) {
            datetime.Year = CalendarYrToTm(year(ptr_time_data));
            datetime.Month = month(ptr_time_data);
            datetime.Day = day(ptr_time_data) + 1;
            datetime.Hour = 0;
            datetime.Minute = 0;
            datetime.Second = 0;
            ptr_time_data = makeTime(datetime);
            ptr_time_data -= readable_configuration.report_seconds;
            is_ptr_updated = true;
            mqtt_state = MQTT_PTR_END;
            SERIAL_TRACE("MQTT_PTR_FOUND ---> MQTT_PTR_END\r\n");
         }
         else {
            is_eof_data_read = false;
            mqtt_state = MQTT_PTR_END;
            SERIAL_TRACE("MQTT_PTR_FOUND ---> MQTT_PTR_END\r\n");
         }
      break;

      case MQTT_PTR_END:
         // ptr data is found: send data saved on sdcard
         if (is_ptr_found && is_sdcard_open && !is_sdcard_error) {
            SERIAL_INFO("Data pointer... [ %02u/%02u/%04u %02u:%02u:%02u ] [ %s ]\r\n", day(ptr_time_data), month(ptr_time_data), year(ptr_time_data), hour(ptr_time_data), minute(ptr_time_data), second(ptr_time_data), OK_STRING);
            mqtt_state = MQTT_OPEN;
            SERIAL_TRACE("MQTT_PTR_END ---> MQTT_OPEN\r\n");
         }
         // ptr data is NOT found: sd card fault fallback: send last acquired sensor data
         else {
            SERIAL_INFO("Data pointer... [ --/--/---- --:--:-- ] [ %s ]\r\n", ERROR_STRING);
            is_sdcard_error = true;
            mqtt_state = MQTT_OPEN;
            SERIAL_TRACE("MQTT_PTR_END ---> MQTT_OPEN\r\n");
         }
      break;

      case MQTT_OPEN:
         if (is_client_connected && mqtt_client.isConnected()) {
            mqtt_state = MQTT_CHECK;
            SERIAL_TRACE("MQTT_OPEN ---> MQTT_CHECK\r\n");
         }
         else if (is_client_connected) {
            ipstack_timeout_ms = 0;
            mqtt_state = MQTT_CONNECT;
            SERIAL_TRACE("MQTT_OPEN ---> MQTT_CONNECT\r\n");
         }
         // error: client not connected!
         else {
            is_mqtt_error = true;
            mqtt_state = MQTT_END;
            SERIAL_TRACE("MQTT_OPEN ---> MQTT_END\r\n");
         }
         break;

      case MQTT_CONNECT:
         if (ipstack_timeout_ms == 0) {
            ipstack_timeout_ms = millis();
         }

         ipstack_status = ipstack.connect(readable_configuration.mqtt_server, readable_configuration.mqtt_port);

         // success
         if (ipstack_status == 1 && mqttConnect(readable_configuration.mqtt_username, readable_configuration.mqtt_password)) {
            retry = 0;
            SERIAL_DEBUG("MQTT Connection... [ %s ]\r\n", OK_STRING);
            mqtt_state = MQTT_ON_CONNECT;
            SERIAL_TRACE("MQTT_CONNECT ---> MQTT_ON_CONNECT\r\n");
         }
         // retry
         else if (ipstack_status == 2 && (++retry) < MQTT_RETRY_COUNT_MAX) {
            delay_ms = MQTT_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = MQTT_CONNECT;
            mqtt_state = MQTT_WAIT_STATE;
            SERIAL_TRACE("MQTT_CONNECT ---> MQTT_WAIT_STATE\r\n");
         }
         // fail
         #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
         else if (ipstack_status == 2 || (millis() - ipstack_timeout_ms >= ETHERNET_MQTT_TIMEOUT_MS)) {
         #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
         else if (ipstack_status == 2) {
         #endif
            SERIAL_ERROR("MQTT Connection... [ %s ]\r\n", FAIL_STRING);
            is_mqtt_error = true;
            mqtt_state = MQTT_ON_DISCONNECT;
            SERIAL_TRACE("MQTT_CONNECT ---> MQTT_ON_DISCONNECT\r\n");
         }
         // wait
      break;

      case MQTT_ON_CONNECT:
         getFullTopic(full_topic_buffer, readable_configuration.mqtt_root_topic, MQTT_STATUS_TOPIC);
         snprintf(&message_buffer[0][0], MQTT_MESSAGE_LENGTH, "{\"v\":\"conn\"}");

         if (mqttPublish(full_topic_buffer, &message_buffer[k][0])) {
            retry = 0;
            mqtt_state = MQTT_SUBSCRIBE;
            SERIAL_TRACE("MQTT_ON_CONNECT ---> MQTT_SUBSCRIBE\r\n");
         }
         // retry
         else if ((++retry) < MQTT_RETRY_COUNT_MAX) {
            delay_ms = MQTT_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = MQTT_ON_CONNECT;
            mqtt_state = MQTT_WAIT_STATE;
            SERIAL_TRACE("MQTT_ON_CONNECT ---> MQTT_WAIT_STATE\r\n");
         }
         // fail
         else {
            retry = 0;
            SERIAL_ERROR("MQTT on connect publish message... [ %s ]\r\n", FAIL_STRING);
            is_mqtt_error = true;
            mqtt_state = MQTT_ON_DISCONNECT;
            SERIAL_TRACE("MQTT_ON_CONNECT ---> MQTT_ON_DISCONNECT\r\n");
         }
      break;

      case MQTT_SUBSCRIBE:
         if (!is_mqtt_subscribed) {
            is_mqtt_subscribed = (mqtt_client.subscribe(readable_configuration.mqtt_subscribe_topic, MQTT::QOS1, mqttRxCallback) == 0);
            is_mqtt_error = !is_mqtt_subscribed;
            SERIAL_DEBUG("MQTT Subscription... [ %s ]\r\n", is_mqtt_subscribed ? OK_STRING : FAIL_STRING);
         }

         mqtt_state = MQTT_CHECK;
         SERIAL_TRACE("MQTT_SUBSCRIBE ---> MQTT_CHECK\r\n");
      break;

      case MQTT_CHECK:
         // ptr data is found: send data saved on sdcard
         if (!is_sdcard_error) {
            is_mqtt_processing_json = false;
            is_mqtt_processing_sdcard = true;
            is_eof_data_read = false;
            mqtt_state = MQTT_OPEN_DATA_FILE;
            SERIAL_TRACE("MQTT_CHECK ---> MQTT_OPEN_DATA_FILE\r\n");
         }
         // ptr data is NOT found: sd card fault fallback: send last acquired sensor data
         else {
            is_mqtt_processing_json = true;
            is_mqtt_processing_sdcard = false;
            i = 0;
            mqtt_state = MQTT_SENSORS_LOOP;
            SERIAL_TRACE("MQTT_CHECK ---> MQTT_SENSORS_LOOP\r\n");
         }
      break;

      case MQTT_SENSORS_LOOP:
         if (i < readable_configuration.sensors_count) {
            k = 0;
            data_count = jsonToMqtt(&json_sensors_data[i][0], readable_configuration.sensors[i].mqtt_topic, topic_buffer, message_buffer, (tmElements_t *) &sensor_reading_time);
            mqtt_state = MQTT_DATA_LOOP;
            SERIAL_TRACE("MQTT_SENSORS_LOOP ---> MQTT_DATA_LOOP\r\n");
         }
         else if (is_mqtt_processing_json) {
            mqtt_state = MQTT_ON_DISCONNECT;
            SERIAL_TRACE("MQTT_SENSORS_LOOP ---> MQTT_ON_DISCONNECT\r\n");
         }
      break;

      case MQTT_SD_LOOP:
         memset(sd_buffer, 0, MQTT_SENSOR_TOPIC_LENGTH + MQTT_MESSAGE_LENGTH);
         memset(topic_buffer, 0, sizeof(topic_buffer[0][0]) * VALUES_TO_READ_FROM_SENSOR_COUNT * MQTT_SENSOR_TOPIC_LENGTH);
         memset(message_buffer, 0, sizeof(message_buffer[0][0]) * VALUES_TO_READ_FROM_SENSOR_COUNT * MQTT_MESSAGE_LENGTH);

         read_bytes_count = read_data_file.read(sd_buffer, MQTT_SENSOR_TOPIC_LENGTH + MQTT_MESSAGE_LENGTH);

         if (read_bytes_count == MQTT_SENSOR_TOPIC_LENGTH + MQTT_MESSAGE_LENGTH) {
            sdToMqtt(sd_buffer, &topic_buffer[0][0], &message_buffer[0][0]);
            current_ptr_time_data = getDateFromMessage(&message_buffer[0][0]);

            if (current_ptr_time_data >= ptr_time_data) {
               last_correct_ptr_time_data = current_ptr_time_data;
               mqtt_state = MQTT_DATA_LOOP;
               SERIAL_TRACE("MQTT_SD_LOOP ---> MQTT_DATA_LOOP\r\n");
            }
         }
         // EOF: End of File
         else {
            if (last_correct_ptr_time_data > ptr_time_data) {
               ptr_time_data = last_correct_ptr_time_data;
               is_ptr_updated = true;
            }
            is_eof_data_read = true;
            mqtt_state = MQTT_PTR_FOUND;
            SERIAL_TRACE("MQTT_SD_LOOP ---> MQTT_PTR_FOUND\r\n");
         }
      break;

      case MQTT_DATA_LOOP:
         if ((k < data_count && is_mqtt_processing_json) || is_mqtt_processing_sdcard) {
            getFullTopic(full_topic_buffer, readable_configuration.mqtt_root_topic, &topic_buffer[k][0]);
            mqtt_state = MQTT_PUBLISH;
            SERIAL_TRACE("MQTT_DATA_LOOP ---> MQTT_PUBLISH\r\n");
         }
         else {
            i++;
            mqtt_state = MQTT_SENSORS_LOOP;
            SERIAL_TRACE("MQTT_DATA_LOOP ---> MQTT_SENSORS_LOOP\r\n");
         }
      break;

      case MQTT_PUBLISH:
         is_mqtt_published_data = true;

         // mqtt json success
         if (is_mqtt_processing_json && mqttPublish(full_topic_buffer, &message_buffer[k][0])) {
            SERIAL_DEBUG("MQTT <-- %s %s\r\n", &topic_buffer[k][0], &message_buffer[k][0]);
            retry = 0;
            k++;
            mqtt_data_count++;
            mqtt_state = MQTT_DATA_LOOP;
            SERIAL_TRACE("MQTT_PUBLISH ---> MQTT_DATA_LOOP\r\n");
         }
         // mqtt sdcard success
         else if (is_mqtt_processing_sdcard && mqttPublish(full_topic_buffer, &message_buffer[0][0])) {
            SERIAL_DEBUG("MQTT <-- %s %s\r\n", &topic_buffer[0][0], &message_buffer[0][0]);
            retry = 0;
            mqtt_data_count++;
            mqtt_state = MQTT_SD_LOOP;
            SERIAL_TRACE("MQTT_PUBLISH ---> MQTT_SD_LOOP\r\n");
         }
         // retry
         else if ((++retry) < MQTT_RETRY_COUNT_MAX) {
            delay_ms = MQTT_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = MQTT_PUBLISH;
            mqtt_state = MQTT_WAIT_STATE;
            SERIAL_TRACE("MQTT_PUBLISH ---> MQTT_WAIT_STATE\r\n");
         }
         // fail
         else {
            ptr_time_data = current_ptr_time_data - readable_configuration.report_seconds;
            is_ptr_updated = true;

            is_eof_data_read = true;
            is_mqtt_error = true;
            SERIAL_ERROR("MQTT publish... [ %s ]\r\n", FAIL_STRING);

            if (is_mqtt_processing_json) {
               mqtt_state = MQTT_ON_DISCONNECT;
               SERIAL_TRACE("MQTT_PUBLISH ---> MQTT_ON_DISCONNECT\r\n");
            }
            else if (is_mqtt_processing_sdcard) {
               mqtt_state = MQTT_CLOSE_DATA_FILE;
               SERIAL_TRACE("MQTT_PUBLISH ---> MQTT_CLOSE_DATA_FILE\r\n");
            }
         }
      break;

      case MQTT_OPEN_DATA_FILE:
         // open the file that corresponds to the next data to send
         next_ptr_time_data = ptr_time_data + (uint8_t)(readable_configuration.report_seconds / 60) * 60;
         sdcard_make_filename(next_ptr_time_data, file_name);

         // open file for read data
         if (sdcard_open_file(&SD, &read_data_file, file_name, O_READ)) {
            retry = 0;
            mqtt_state = MQTT_SD_LOOP;
            SERIAL_TRACE("MQTT_OPEN_DATA_FILE ---> MQTT_SD_LOOP\r\n");
         }
         // error: file doesn't exist but if is today, end.
         else if (!is_sdcard_error && year(next_ptr_time_data) == year() && month(next_ptr_time_data) == month() && day(next_ptr_time_data) == day()) {
            mqtt_state = MQTT_PTR_UPDATE;
            SERIAL_TRACE("MQTT_OPEN_DATA_FILE ---> MQTT_PTR_UPDATE\r\n");
         }
         // error: file doesn't exist and if it isn't today, jump to next day and search in it
         else if (!is_sdcard_error) {
            is_ptr_found = false;
            ptr_time_data = next_ptr_time_data;
            mqtt_state = MQTT_PTR_FIND;
            SERIAL_TRACE("MQTT_OPEN_DATA_FILE ---> MQTT_PTR_FIND\r\n");
         }
         // fail
         else {
            SERIAL_ERROR("SD Card open file %s... [ FAIL ]\r\n", file_name);
            is_sdcard_error = true;
            mqtt_state = MQTT_CHECK; // fallback
            SERIAL_TRACE("MQTT_OPEN_DATA_FILE ---> MQTT_CHECK\r\n");
         }
         break;

      case MQTT_CLOSE_DATA_FILE:
         if (is_mqtt_processing_sdcard) {
            is_sdcard_error = !read_data_file.close();
            mqtt_state = MQTT_ON_DISCONNECT;
            SERIAL_TRACE("MQTT_CLOSE_DATA_FILE ---> MQTT_ON_DISCONNECT\r\n");
         }
         break;

      case MQTT_ON_DISCONNECT:
         getFullTopic(full_topic_buffer, readable_configuration.mqtt_root_topic, MQTT_STATUS_TOPIC);
         snprintf(&message_buffer[0][0], MQTT_MESSAGE_LENGTH, "{\"v\":\"disconn\"}");

         if (mqttPublish(full_topic_buffer, &message_buffer[k][0])) {
            retry = 0;
            mqtt_state = MQTT_DISCONNECT;
            SERIAL_TRACE("MQTT_ON_DISCONNECT ---> MQTT_DISCONNECT\r\n");
         }
         // retry
         else if ((++retry) < MQTT_RETRY_COUNT_MAX) {
            delay_ms = MQTT_DELAY_MS;
            start_time_ms = millis();
            state_after_wait = MQTT_ON_DISCONNECT;
            mqtt_state = MQTT_WAIT_STATE;
            SERIAL_TRACE("MQTT_ON_DISCONNECT ---> MQTT_WAIT_STATE\r\n");
         }
         // fail
         else {
            SERIAL_ERROR("MQTT on disconnect publish message... [ %s ]\r\n", FAIL_STRING);
            retry = 0;
            is_mqtt_error = true;
            mqtt_state = MQTT_DISCONNECT;
            SERIAL_TRACE("MQTT_ON_DISCONNECT ---> MQTT_DISCONNECT\r\n");
         }
      break;

      case MQTT_DISCONNECT:
         #if (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_ETH || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_ETH)
         // if (is_mqtt_error) {
         mqtt_client.disconnect();
         ipstack.disconnect();
         SERIAL_DEBUG("MQTT Disconnect... [ %s ]\r\n", OK_STRING);
         // }

         #elif (MODULE_TYPE == STIMA_MODULE_TYPE_SAMPLE_GSM || MODULE_TYPE == STIMA_MODULE_TYPE_REPORT_GSM)
         mqtt_client.disconnect();
         ipstack.disconnect();
         SERIAL_DEBUG("MQTT Disconnect... [ %s ]\r\n", OK_STRING);

         // resume GSM task for closing connection
         noInterrupts();
         if (!is_event_gsm) {
            is_event_gsm = true;
            ready_tasks_count++;
         }
         interrupts();

         #endif

         mqtt_state = MQTT_PTR_UPDATE;
         SERIAL_TRACE("MQTT_DISCONNECT ---> MQTT_PTR_UPDATE\r\n");

      break;

      case MQTT_PTR_UPDATE:
         if (is_ptr_updated) {
            // set ptr 1 second more for send next data to current ptr
            ptr_time_data++;

            // success
            if (mqtt_ptr_file.seekSet(0) && mqtt_ptr_file.write(&ptr_time_data, sizeof(time_t)) == sizeof(time_t)) {
               mqtt_ptr_file.flush();
               breakTime(ptr_time_data, datetime);
               SERIAL_INFO("Data pointer... [ %02u/%02u/%04u %02u:%02u:%02u ] [ %s ]\r\n", datetime.Day, datetime.Month, tmYearToCalendar(datetime.Year), datetime.Hour, datetime.Minute, datetime.Second, "UPDATE");
               mqtt_state = MQTT_CLOSE_PTR_FILE;
               SERIAL_TRACE("MQTT_PTR_UPDATE ---> MQTT_CLOSE_PTR_FILE\r\n");
            }
            // retry
            else if ((++retry) < MQTT_RETRY_COUNT_MAX) {
               delay_ms = MQTT_DELAY_MS;
               start_time_ms = millis();
               state_after_wait = MQTT_PTR_UPDATE;
               mqtt_state = MQTT_WAIT_STATE;
               SERIAL_TRACE("MQTT_PTR_UPDATE ---> MQTT_WAIT_STATE\r\n");
            }
            // fail
            else {
               SERIAL_ERROR("SD Card writing ptr data on file %s... [ %s ]\r\n", SDCARD_MQTT_PTR_FILE_NAME, FAIL_STRING);
               mqtt_state = MQTT_CLOSE_PTR_FILE;
               SERIAL_TRACE("MQTT_PTR_UPDATE ---> MQTT_CLOSE_PTR_FILE\r\n");
            }
         }
         else {
            mqtt_state = MQTT_CLOSE_PTR_FILE;
            SERIAL_TRACE("MQTT_PTR_UPDATE ---> MQTT_CLOSE_PTR_FILE\r\n");
         }
         break;

      case MQTT_CLOSE_PTR_FILE:
         mqtt_ptr_file.close();
         mqtt_state = MQTT_CLOSE_SDCARD;
         SERIAL_TRACE("MQTT_CLOSE_PTR_FILE ---> MQTT_CLOSE_SDCARD\r\n");
         break;

      case MQTT_CLOSE_SDCARD:
         is_sdcard_error = false;
         is_sdcard_open = false;
         mqtt_state = MQTT_END;
         SERIAL_TRACE("MQTT_CLOSE_SDCARD ---> DATA_PROCESSING_END\r\n");
         break;

      case MQTT_END:
         if (is_mqtt_published_data) {
            SERIAL_INFO("[ %u ] data published through mqtt... [ %s ]\r\n", mqtt_data_count, is_mqtt_error ? ERROR_STRING : OK_STRING);
            LCD_INFO(&lcd, false, "mqtt %u data %s", mqtt_data_count, is_mqtt_error ? ERROR_STRING : OK_STRING);
         }

         noInterrupts();
         is_event_mqtt_paused = false;
         is_event_mqtt = false;
         ready_tasks_count--;
         interrupts();

         mqtt_state = MQTT_INIT;
         SERIAL_TRACE("MQTT_END ---> MQTT_INIT\r\n");
      break;

      case MQTT_WAIT_STATE:
         if (millis() - start_time_ms > delay_ms) {
            mqtt_state = state_after_wait;
         }
      break;
   }
}

bool stream_task(Stream *stream, uint32_t stream_timeout, uint32_t end_task_timeout) {
   static bool is_end;
   static uint32_t start_time_ms;
   static char buffer[STREAM_BUFFER_LENGTH];
   char *response;

   switch (stream_state) {
      case STREAM_INIT:
         is_end = false;
         start_time_ms = 0;
         memset(buffer, 0, STREAM_BUFFER_LENGTH);
         stream->flush();
         stream->setTimeout(stream_timeout);
         stream_state = STREAM_AVAILABLE;
      break;

      case STREAM_AVAILABLE:
         if (stream->available()) {
            start_time_ms = millis();
            stream->readBytes(buffer, STREAM_BUFFER_LENGTH);
            stream_state = STREAM_PROCESS;
         }
         else if (start_time_ms && end_task_timeout && (millis() - start_time_ms > end_task_timeout)) {
            stream_state = STREAM_END;
         }
      break;

      case STREAM_PROCESS:
         response = rpc_process(buffer);

         if (strlen(buffer)) {
            stream->write(response);
         }

         stream_state = STREAM_AVAILABLE;
      break;

      case STREAM_END:
         noInterrupts();
         if (is_event_stream) {
            is_event_stream = false;
            ready_tasks_count--;
         }
         interrupts();
         stream_state = STREAM_INIT;
         is_end = true;
      break;
   }

   return is_end;
}
