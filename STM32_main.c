/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* --- UMBRALES DE ALARMA --- */
#define SPO2_MIN 90      // Menos de 90% es hipoxia
#define HR_MIN 40        // Bradicardia extrema
#define HR_MAX 130       // Taquicardia
#define FS 500.0f
#define FC 50.0f
#define PI 3.14159265f
#define MAX30102_ADDR (0x57 << 1)
#define BUFFER_SIZE 100

/* VARIABLES GLOBALES */
ADC_HandleTypeDef hadc1;
UART_HandleTypeDef huart2;
I2C_HandleTypeDef hi2c1;
TIM_HandleTypeDef htim2; // Variable para el Timer de la alarma

/* Variables Filtro ECG */
float alpha;
float y_prev = 0.0f;

/* Variables Oxímetro */
uint32_t redBuffer[BUFFER_SIZE];
uint32_t irBuffer[BUFFER_SIZE];
uint8_t bufferIndex = 0;

/* Estado de Alarma (Global para que la interrupción lo vea) */
volatile uint8_t alarma_activa = 0; // 0 = Todo bien, 1 = ALERTA

/* PROTOTIPOS */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM2_Init(void); // Nueva función del Timer
void Error_Handler(void);

void MAX30102_Init(void);
void MAX30102_ReadFIFO(uint32_t *red, uint32_t *ir);
float CalculateSpO2(void);
float CalculateHR(void);

/* --- MAIN --- */
int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_USART2_UART_Init();
  MX_I2C1_Init();
  MX_TIM2_Init(); // Inicializar Timer

  /* INICIAR TIMER EN MODO INTERRUPCIÓN */
  HAL_TIM_Base_Start_IT(&htim2);

  MAX30102_Init();

  // Llenado inicial buffer
  for(uint8_t i = 0; i < BUFFER_SIZE; i++) {
      MAX30102_ReadFIFO(&redBuffer[i], &irBuffer[i]);
      HAL_Delay(5);
  }

  // Calculo Alpha
  float dt = 1.0f / FS;
  float rc = 1.0f / (2.0f * PI * FC);
  alpha = dt / (rc + dt);

  char buffer_msg[64];
  uint32_t last_spo2_update = 0;
  float spo2 = 0;
  float hr = 0;
  uint32_t redVal, irVal;

  /* Bucle Infinito */
  while (1)
  {
    /* A. ECG (Prioridad Alta) */
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 2) == HAL_OK)
    {
        float x = (float)HAL_ADC_GetValue(&hadc1);
        float y = y_prev + alpha * (x - y_prev);
        y_prev = y;

        sprintf(buffer_msg, "%d\n", (int)y);
        HAL_UART_Transmit(&huart2, (uint8_t*)buffer_msg, strlen(buffer_msg), 10);
    }

    /* B. MAX30102 */
    MAX30102_ReadFIFO(&redVal, &irVal);
    for(int i = 0; i < BUFFER_SIZE - 1; i++) {
        redBuffer[i] = redBuffer[i+1];
        irBuffer[i]  = irBuffer[i+1];
    }
    redBuffer[BUFFER_SIZE - 1] = redVal;
    irBuffer[BUFFER_SIZE - 1] = irVal;

    /* C. LÓGICA DE ALARMA (Cada 1 seg) */
    if (HAL_GetTick() - last_spo2_update > 1000)
    {
        spo2 = CalculateSpO2();
        hr = CalculateHR();

        // 1. Verificar si hay dedo (Si IR es muy bajo, no hay dedo)
        uint32_t avgIR = 0;
        for(int k=0; k<BUFFER_SIZE; k++) avgIR += irBuffer[k];
        avgIR /= BUFFER_SIZE;

        if (avgIR < 10000) {
            // No hay dedo: Apagar alarmas, encender LED OK (o ambos apagados)
            alarma_activa = 0;
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_SET); // Verde ON
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET); // Rojo OFF
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET); // Buzzer OFF
        }
        else {
            // 2. Verificar valores anormales
            if (spo2 < SPO2_MIN || hr < HR_MIN || hr > HR_MAX) {
                alarma_activa = 1; // Activa la interrupción del buzzer
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_RESET); // Verde OFF
                // El Rojo lo controla el Timer para que parpadee junto al buzzer
            } else {
                alarma_activa = 0; // Todo bien
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3, GPIO_PIN_SET);   // Verde ON
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET); // Rojo OFF
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET); // Buzzer OFF
            }
        }

        // Enviar datos a ESP32
        sprintf(buffer_msg, "S:%d,%d\n", (int)spo2, (int)hr);
        HAL_UART_Transmit(&huart2, (uint8_t*)buffer_msg, strlen(buffer_msg), 100);

        last_spo2_update = HAL_GetTick();
    }

    HAL_Delay(2);
  }
}

/* =================================================================
   INTERRUPCIÓN DEL TIMER (Esto ocurre "en segundo plano")
   Se ejecuta cada vez que el Timer se desborda (aprox cada 500ms)
   ================================================================= */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  // Chequeamos que sea el TIM2
  if (htim->Instance == TIM2) {
      if (alarma_activa == 1) {
          // Si hay alarma, invertimos el estado (Toggle) para hacer Bip-Bip
          HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0); // Pin Buzzer
          HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_1); // Pin LED Rojo
      } else {
          // Si no hay alarma, nos aseguramos que estén apagados
          HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET);
      }
  }
}

/* -----------------------------------------------------------------
   RESTO DE FUNCIONES (MAX30102, INITS)
   ----------------------------------------------------------------- */
void MAX30102_Init(void)
{
    uint8_t reg, data;
    reg = 0x09; data = 0x40; HAL_I2C_Mem_Write(&hi2c1, MAX30102_ADDR, reg, 1, &data, 1, 100); HAL_Delay(10);
    reg = 0x09; data = 0x03; HAL_I2C_Mem_Write(&hi2c1, MAX30102_ADDR, reg, 1, &data, 1, 100);
    reg = 0x0A; data = 0x27; HAL_I2C_Mem_Write(&hi2c1, MAX30102_ADDR, reg, 1, &data, 1, 100);
    reg = 0x0C; data = 0x24; HAL_I2C_Mem_Write(&hi2c1, MAX30102_ADDR, reg, 1, &data, 1, 100);
    reg = 0x0D; data = 0x24; HAL_I2C_Mem_Write(&hi2c1, MAX30102_ADDR, reg, 1, &data, 1, 100);
}

void MAX30102_ReadFIFO(uint32_t *red, uint32_t *ir)
{
    uint8_t rawData[6];
    HAL_I2C_Mem_Read(&hi2c1, MAX30102_ADDR, 0x07, 1, rawData, 6, 10);
    *red = ((uint32_t)(rawData[0] << 16) | (uint32_t)(rawData[1] << 8) | rawData[2]) & 0x03FFFF;
    *ir  = ((uint32_t)(rawData[3] << 16) | (uint32_t)(rawData[4] << 8) | rawData[5]) & 0x03FFFF;
}

float CalculateSpO2(void)
{
    uint64_t sumRed=0, sumIR=0;
    for(uint8_t i=0;i<BUFFER_SIZE;i++) { sumRed += redBuffer[i]; sumIR += irBuffer[i]; }
    if(sumIR==0) return 0;
    float ratio = (float)sumRed / (float)sumIR;
    float calc_spo2 = 110.0 - 25.0*ratio;
    if(calc_spo2>100) calc_spo2=100; else if(calc_spo2<0) calc_spo2=0;
    return calc_spo2;
}

float CalculateHR(void)
{
    uint32_t irAvg = 0; uint8_t peaks = 0; int lastPeakIndex = -50;
    for(uint8_t i = 0; i < BUFFER_SIZE; i++) irAvg += irBuffer[i];
    irAvg /= BUFFER_SIZE;
    if(irAvg < 10000) return 0;
    for(uint8_t i = 1; i < BUFFER_SIZE - 1; i++) {
        if(irBuffer[i] > irAvg && irBuffer[i] > irBuffer[i-1] && irBuffer[i] > irBuffer[i+1]) {
            if((i - lastPeakIndex) > 25) { peaks++; lastPeakIndex = i; }
        }
    }
    return (float)peaks * 60.0f;
}

/* --- INIT FUNCTIONS --- */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) Error_Handler();
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSE|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = 0;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_6;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_MSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK) Error_Handler();
  HAL_RCCEx_EnableMSIPLLMode();
}

static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2; huart2.Init.BaudRate = 115200; huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1; huart2.Init.Parity = UART_PARITY_NONE; huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE; huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  hadc1.Instance = ADC1; hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1; hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT; hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE; hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE; hadc1.Init.ContinuousConvMode = DISABLE; hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE; hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE; hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED; hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();
  sConfig.Channel = ADC_CHANNEL_5; sConfig.Rank = ADC_REGULAR_RANK_1; sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED; sConfig.OffsetNumber = ADC_OFFSET_NONE; sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) Error_Handler();
}

static void MX_I2C1_Init(void) {
    hi2c1.Instance = I2C1; hi2c1.Init.Timing = 0x00100D14; hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT; hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0; hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE; hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) Error_Handler();
    HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE);
    HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0);
}

/* --- CONFIGURACIÓN DEL TIMER 2 --- */
static void MX_TIM2_Init(void)
{
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 8000 - 1;   // Reloj lento
  htim2.Init.Period = 5000 - 1; // Disparo cada 500ms aprox
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK) Error_Handler();

  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK) Error_Handler();

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /* Configurar Salidas de Alarma (PB0, PB1, PB3) */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_3, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}
