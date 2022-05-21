#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "CANopen.h"
#include "301/CO_config.h"
#include "OD.h"

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_log.h"

#include "config.h"


// Periodic CAN Timer Task
volatile uint32_t coInterruptCounter = 0U;
static void app_can_timer_task(void *arg);
esp_timer_create_args_t coMainTaskArgs;
esp_timer_handle_t periodicTimer;

CO_t *CO = NULL; /* CANopen object */
uint32_t heapMemoryUsed;
CO_config_t *config_ptr = NULL;

uint8_t LED_red, LED_green;

// LinMot CMD
OD_extension_t OD_variable_linMotCMDParameters_extension = {
    .object = NULL,
    .read = OD_readOriginal,
    .write = OD_writeOriginal
};
uint8_t *OD_variable_linMotCMDParameters_flagsPDO = NULL;

// LinMot Control Word
OD_extension_t OD_variable_linMotControlWord_extension = {
    .object = NULL,
    .read = OD_readOriginal,
    .write = OD_writeOriginal
};
uint8_t *OD_variable_linMotControlWord_flagsPDO = NULL;

// Linmot Heartbeat via RPDO
volatile uint32_t linmotPDOCounter = 0U;
ODR_t track_status_update(
  OD_stream_t *stream, const void *buf,
  OD_size_t count, OD_size_t *countWritten
) {
  linmotPDOCounter = 0;
  return OD_writeOriginal(stream, buf, count, countWritten);
}

OD_extension_t OD_variable_linMotStatus_extension = {
    .object = NULL,
    .read = OD_readOriginal,
    .write = track_status_update
};

void app_init_communication() {
  ESP_LOGI("init.comm", "Resetting communication...");
  CO_ReturnError_t err;
  uint32_t errInfo = 0;

  CO->CANmodule->CANnormal = false;
  // CO_CANsetConfigurationMode((void *)&CANptr); - Not needed on ESP32 - We handle this in CO_CANinit
  // TODO - ESP_ERR_INVALID_STATE - CO_CANmodule_disable(CO->CANmodule);

  /* Initialize CAN Device */
  err = CO_CANinit(CO, NULL, CAN_BITRATE /* bit rate */);
  if (err != CO_ERROR_NO)
  {
    ESP_LOGE("init.comm", "CO_init failed. Errorcode: %d", err);
    CO_errorReport(CO->em, CO_EM_MEMORY_ALLOCATION_ERROR, CO_EMC_SOFTWARE_INTERNAL, err);
    esp_restart();
  }

  /* Initialize CANOpen services as defined in CO_config.h */
  err = CO_CANopenInit(
    CO,                /* CANopen object */
    NULL,              /* alternate NMT */
    NULL,              /* alternate em */
    OD,                /* Object dictionary */
    OD_STATUS_BITS,    /* Optional OD_statusBits */
    NMT_CONTROL,       /* CO_NMT_control_t */
    FIRST_HB_TIME,     /* firstHBTime_ms */
    SDO_SRV_TIMEOUT_TIME, /* SDOserverTimeoutTime_ms */
    SDO_CLI_TIMEOUT_TIME, /* SDOclientTimeoutTime_ms */
    SDO_CLI_BLOCK,     /* SDOclientBlockTransfer */
    NODE_ID_SELF,
    &errInfo
  );

  if(err != CO_ERROR_NO && err != CO_ERROR_NODE_ID_UNCONFIGURED_LSS) {
    if (err == CO_ERROR_OD_PARAMETERS) {
      ESP_LOGE("init.comm", "Error: Object Dictionary entry 0x%X\n", errInfo);
    } else {
      ESP_LOGE("init.comm", "Error: CANopen initialization failed: %d\n", err);
    }
    return;
  }

  // Initialize LinMot CMD Parameters as TPDO Mapped Object
  //  - Must be before CO_CANopenInitPDO
  //  - Allows using OD_requestTPDO for triggering
  OD_extension_init(
    OD_ENTRY_H2113_linMotCMD_Parameters,
    &OD_variable_linMotCMDParameters_extension
  );
  OD_variable_linMotCMDParameters_flagsPDO = OD_getFlagsPDO(OD_ENTRY_H2113_linMotCMD_Parameters);

  // Initialize LinMot Control Word as TPDO Mapped Object
  OD_extension_init(
    OD_ENTRY_H2111_linMotControlWord,
    &OD_variable_linMotControlWord_extension
  );
  OD_variable_linMotControlWord_flagsPDO = OD_getFlagsPDO(OD_ENTRY_H2111_linMotControlWord);

  // Initialize LinMot Status Words as RPDO Mapped Object
  //  - Used as a pesudo-heartbeat to detect alive-ness of LinMot
  OD_extension_init(
    OD_ENTRY_H2110_linMotStatus,
    &OD_variable_linMotStatus_extension
  );

  /* Initialize TPDO and RPDO if defined in CO_config.h */
  err = CO_CANopenInitPDO(CO, CO->em, OD, NODE_ID_SELF, &errInfo);
  if(err != CO_ERROR_NO) {
    if (err == CO_ERROR_OD_PARAMETERS) {
      ESP_LOGE("init.comm", "Error: Object Dictionary entry 0x%X\n", errInfo);
    } else {
      ESP_LOGE("init.comm", "Error: PDO initialization failed: %d\n", err);
    }
    return;
  }

  /* Configure Timer interrupt function for execution every CO_MAIN_TASK_INTERVAL */
  coMainTaskArgs.callback = &app_can_timer_task;
  coMainTaskArgs.name = "app_can_timer_task";
  ESP_ERROR_CHECK(esp_timer_create(&coMainTaskArgs, &periodicTimer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(periodicTimer, CO_MAIN_TASK_INTERVAL));

  /* start CAN */
  CO_CANsetNormalMode(CO->CANmodule);

  ESP_LOGI("init.comm", "done");
}

void app_loop() {

}

CO_NMT_reset_cmd_t reset = CO_RESET_COMM;
bool hasInitMot = false;
void app_can_main_task(void *pvParameter) {
  CO = CO_new(config_ptr, &heapMemoryUsed);
  if (CO == NULL) {
    ESP_LOGE("task.main", "Error: Can't allocate memory\n");
    return;
  } else {
    ESP_LOGI("task.main", "Allocated %u bytes for CANopen objects\n", heapMemoryUsed);
  }

  vTaskDelay(BOOT_WAIT / portTICK_PERIOD_MS);
  ESP_LOGI("task.main", "Started");

  while (reset != CO_RESET_APP) {
    uint32_t coInterruptCounterPrevious = coInterruptCounter;
    app_init_communication();
    reset = CO_RESET_NOT;

    while (reset == CO_RESET_NOT) {
      uint32_t coInterruptCounterCopy;
      uint32_t coInterruptCounterDiff;
      coInterruptCounterCopy = coInterruptCounter;
      coInterruptCounterDiff = coInterruptCounterCopy - coInterruptCounterPrevious;
      coInterruptCounterPrevious = coInterruptCounterCopy;

      //ESP_LOGI("task.main", "Processing CANOpen... %u diff cycles", coInterruptCounterDiff);
      reset = CO_process(CO, false, coInterruptCounterDiff * 1000, NULL);
      app_loop();

      vTaskDelay(MAIN_WAIT / portTICK_PERIOD_MS);
    }

    ESP_LOGI("task.main", "Communication reset was requested by CANOpen");
  }

  ESP_LOGI("task.main", "Application reset was requested by CANOpen");
  esp_restart();
}

// TODO - Somehow integrate into main app task? What happens on app/comm reset?
// TODO - Ensure 16bit mapping is correct in Linmot
#define LINMOT_CONTROL_SWITCH_ON ((uint16_t)(1 << 0))
#define LINMOT_CONTROL_ERROR_ACKNOWLEDGE ((uint16_t)(1 << 7))
#define LINMOT_CONTROL_HOME ((uint16_t)(1 << 11))

#define LINMOT_STATUS_OPERATION_ENABLED (1 << 0)
#define LINMOT_STATUS_SWITCH_ON (1 << 1)
#define LINMOT_STATUS_ERROR (1 << 3)
#define LINMOT_STATUS_VOLTAGE_ENABLE (1 << 4)
#define LINMOT_STATUS_WARNING (1 << 7)
#define LINMOT_STATUS_AT_TARGET (1 << 10)
#define LINMOT_STATUS_HOMED (1 << 11)
#define LINMOT_STATUS_FATAL_ERROR (1 << 12)
#define LINMOT_STATUS_MOTION_ACTIVE (1 << 13)

#define LINMOT_WARN_MOTOR_HOT_SENSOR (1 << 0)
#define LINMOT_WARN_MOTOR_SHORT_TIME_OVERLOAD (1 << 1)
#define LINMOT_WARN_MOTOR_SUPPLY_VOLTAGE_LOW (1 << 2)
#define LINMOT_WARN_MOTOR_SUPPLY_VOLTAGE_HIGH (1 << 3)
#define LINMOT_WARN_MOTOR_DRIVE_HOT (1 << 6)
#define LINMOT_WARN_MOTOR_NOT_HOMED (1 << 7)

#define LINMOT_ERROR_CANBUS_GUARD_TIMEOUT (0xCD)

uint16_t position = 0;
uint8_t cmdCount = 0;
uint16_t controlWord = 0x0000;
static void app_linmot_task(void *pvParameter) {
  while (true) {
    if (reset != CO_RESET_NOT) {

      vTaskDelay(250 / portTICK_PERIOD_MS);
      continue;
    }

    if (hasInitMot == false) {
      /*Set Operating Mode of Slaves to Operational*/
      ESP_LOGI("task.main", "Setting LinMot to Operational State!\n");
      CO_NMT_sendCommand(CO->NMT, CO_NMT_ENTER_OPERATIONAL, NODE_ID_LINMOT);
      hasInitMot = true;

      vTaskDelay(250 / portTICK_PERIOD_MS);
      continue;
    }

    // It's been 1000ms since we've last gotten a LinMot Status update
    if (linmotPDOCounter > 1000) {
      ESP_LOGE("task.main", "Error: Have not recieved LinMot RPDO in %d ms! Attempting to re-establish!\n", linmotPDOCounter);
      CO_NMT_sendCommand(CO->NMT, CO_NMT_ENTER_OPERATIONAL, NODE_ID_LINMOT);

      vTaskDelay(250 / portTICK_PERIOD_MS);
      continue;
    }

    // If not homed, attempt to home
    uint16_t statusWord;
    uint16_t runWord;
    uint16_t errorWord;
    uint16_t warnWord;

    OD_get_u16(OD_ENTRY_H2110_linMotStatus, 0x01, &statusWord, false);
    OD_get_u16(OD_ENTRY_H2110_linMotStatus, 0x02, &runWord, false);
    OD_get_u16(OD_ENTRY_H2110_linMotStatus, 0x03, &errorWord, false);
    OD_get_u16(OD_ENTRY_H2110_linMotStatus, 0x04, &warnWord, false);

    bool isEnabled = (statusWord & LINMOT_STATUS_OPERATION_ENABLED) > 0;
    bool isActive = (statusWord & LINMOT_STATUS_SWITCH_ON) > 0;
    bool isHomed = (statusWord & LINMOT_STATUS_HOMED) > 0;
    bool isMoving = (statusWord & LINMOT_STATUS_MOTION_ACTIVE) > 0;
    bool isError = (statusWord & LINMOT_STATUS_ERROR) > 0;

    if (isError && (errorWord == LINMOT_ERROR_CANBUS_GUARD_TIMEOUT)) {
      ESP_LOGI("task.main", "LinMot is in CanBus Guard Timeout state. Acknowledging!\n");

      controlWord |= LINMOT_CONTROL_ERROR_ACKNOWLEDGE;
      OD_set_u16(OD_ENTRY_H2111_linMotControlWord, 0x00, controlWord, false);
      OD_requestTPDO(OD_variable_linMotControlWord_flagsPDO, 0);

      vTaskDelay(50 / portTICK_PERIOD_MS);

      controlWord &= ~LINMOT_CONTROL_ERROR_ACKNOWLEDGE;
      OD_set_u16(OD_ENTRY_H2111_linMotControlWord, 0x00, controlWord, false);
      OD_requestTPDO(OD_variable_linMotControlWord_flagsPDO, 0);

      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    if (!isEnabled) {
      ESP_LOGI("task.main", "Switching on LinMot!\n");

      controlWord |= LINMOT_CONTROL_SWITCH_ON;
      OD_set_u16(OD_ENTRY_H2111_linMotControlWord, 0x00, controlWord, false);
      OD_requestTPDO(OD_variable_linMotControlWord_flagsPDO, 0);

      vTaskDelay(250 / portTICK_PERIOD_MS);
      continue;
    }

    if (isEnabled && !isHomed && !isMoving) {
      ESP_LOGI("task.main", "Homing LinMot!\n");

      controlWord |= LINMOT_CONTROL_HOME;
      OD_set_u16(OD_ENTRY_H2111_linMotControlWord, 0x00, controlWord, false);
      OD_requestTPDO(OD_variable_linMotControlWord_flagsPDO, 0);

      // TODO - Should shut off homing state after this
      // This is why Motion CMD error is encountered after rebooting drive!

      vTaskDelay(500 / portTICK_PERIOD_MS);
      continue;
    }

    if (isEnabled && isActive && isHomed && !isMoving) {
      ESP_LOGI("task.main", "Sending LinMot Motion CMD!\n");

      // VAI 16Bit Go To Pos (090xh)
      cmdCount = (cmdCount + 1) % 16;
      uint16_t cmd = ((0x09 & 0xFF) << 8) | ((0x00 & 0x0F) << 4) | (cmdCount & 0x0F);
      OD_set_u16(OD_ENTRY_H2112_linMotCMD_Header, 0x00, cmd, false);

      // Target Position : 50mm
      if (position == 0x01F4) {
        position = 0x03E8;
      } else {
        position = 0x01F4;
      }
      OD_set_u8(OD_ENTRY_H2113_linMotCMD_Parameters, 0x01, (uint8_t)((position >> 0) & 0xFF), false);
      OD_set_u8(OD_ENTRY_H2113_linMotCMD_Parameters, 0x02, (uint8_t)((position >> 8) & 0xFF), false);

      // Maximal Velocity : 5m/s
      uint16_t speed = 0x03E8 * 5;
      OD_set_u8(OD_ENTRY_H2113_linMotCMD_Parameters, 0x03, (uint8_t)((speed >> 0) & 0xFF), false);
      OD_set_u8(OD_ENTRY_H2113_linMotCMD_Parameters, 0x04, (uint8_t)((speed >> 8) & 0xFF), false);

      // Acceleration : 25m/s2 
      OD_set_u8(OD_ENTRY_H2113_linMotCMD_Parameters, 0x05, 0xFF, false);
      OD_set_u8(OD_ENTRY_H2113_linMotCMD_Parameters, 0x06, 0x00, false);

      // De-acceleration : 25m/s2 
      OD_set_u8(OD_ENTRY_H2113_linMotCMD_Parameters, 0x07, 0xFF, false);
      OD_set_u8(OD_ENTRY_H2113_linMotCMD_Parameters, 0x08, 0x00, false);

      // Triggers both TDPO 1 & 2
      OD_requestTPDO(OD_variable_linMotCMDParameters_flagsPDO, 0x01);
      OD_requestTPDO(OD_variable_linMotCMDParameters_flagsPDO, 0x07);

      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

static void app_can_timer_task(void *arg) {
  coInterruptCounter++;
  linmotPDOCounter++;

  CO_LOCK_OD(CO->CANmodule);

  if (!CO->nodeIdUnconfigured && CO->CANmodule->CANnormal) {
    bool_t syncWas = false;

#if (CO_CONFIG_SYNC) & CO_CONFIG_SYNC_ENABLE
    syncWas = CO_process_SYNC(CO, CO_MAIN_TASK_INTERVAL, NULL);
#endif

#if (CO_CONFIG_PDO) & CO_CONFIG_RPDO_ENABLE
    CO_process_RPDO(CO, syncWas, CO_MAIN_TASK_INTERVAL, NULL);
#endif

#if (CO_CONFIG_PDO) & CO_CONFIG_TPDO_ENABLE
    CO_process_TPDO(CO, syncWas, CO_MAIN_TASK_INTERVAL, NULL);
#endif
  }

  CO_UNLOCK_OD(CO->CANmodule);
}

void app_main()
{
  ESP_LOGI("boot", "Attempting to start Main Task...");
  xTaskCreate(&app_can_main_task, "app_can_main_task", 4096, NULL, 5, NULL);
  xTaskCreate(&app_linmot_task, "app_linmot_task", 4096, NULL, 5, NULL);
}