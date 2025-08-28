/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "fonts.h"
#include "ssd1306.h"
#include "stdio.h"
#include "string.h"
#include "core_cm3.h"
/* USER CODE END Includes */ 

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum {SYS_SAFE=0, SYS_CAUTION=1, SYS_DANGER=2} sys_status_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define USE_SOIL_SENSOR   1   // <- keep 0 for now; set to 1 after you buy it
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

/* USER CODE BEGIN PV */
// Thresholds (tweak later)
static int TEMP_HIGH_C = 35;
static int HUM_LOW_PCT = 20;
static int HUM_HIGH_PCT = 85;
static int SOIL_DRY_PCT = 20; // only used when USE_SOIL_SENSOR == 1

// Live values
static volatile uint16_t adc_raw = 0;
static volatile int soil_pct = 0;    // 0..100 (only if soil sensor enabled)
static volatile int temp_c = 0;
static volatile int hum_pct = 0;
static volatile int motion = 0;      // 0/1

// For OLED lines
static char l1[22], l2[22], l3[22], l4[22];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);
/* USER CODE BEGIN PFP */
// Timing
static inline void DWT_Delay_Init(void);
static inline void DWT_Delay_us(uint32_t us);

// Sensors
static uint16_t ADC_Read_PA0(void);
static int SoilPercent_From_ADC(uint16_t raw);
static int IR_Read(void);

// Actuators
static void LED_Green(int on);
static void LED_Yellow(int on);
static void LED_Red(int on);
static void Buzzer(int on);

// DHT11 on PA1
static void DHT11_Pin_Output(void);
static void DHT11_Pin_Input(void);
static uint8_t DHT11_ReadByte(void);
static int DHT11_Read_TempHum(int* tC, int* hPct);

// OLED
static void OLED_ShowLines(const char* a, const char* b, const char* c, const char* d);

// Logic
static sys_status_t EvaluateStatus(int tC, int h, int soil, int motionNow);
static void DriveOutputs(sys_status_t st);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/************ Microsecond delay using DWT (Cortex-M3 supports CYCCNT) ************/
static inline void DWT_Delay_Init(void) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline void DWT_Delay_us(uint32_t us) {
  uint32_t cycles = (SystemCoreClock/1000000) * us;
  uint32_t start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < cycles);
}

static uint16_t ADC_Read_PA0(void) {
  HAL_ADC_Start(&hadc1);
  HAL_ADC_PollForConversion(&hadc1, 10);
  uint16_t v = HAL_ADC_GetValue(&hadc1);
  HAL_ADC_Stop(&hadc1);
  return v;
}

static int SoilPercent_From_ADC(uint16_t raw) {
  // flip mapping: wet soil -> high percentage
  int pct = 100- ((int)((raw * 100UL) / 4095UL));
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

int Get_SoilPercent(void) {
  uint32_t sum = 0;
  for(int i=0; i<10; i++) {
    sum += ADC_Read_PA0();
    HAL_Delay(5);
  }
  uint16_t avg = sum / 10;
  return SoilPercent_From_ADC(avg);
}

static int IR_Read(void) {
  return HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0) ? 0 : 1;
}

static void LED_Green(int on){
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
static void LED_Yellow(int on){
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
static void LED_Red(int on){
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
static void Buzzer(int on){
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/************ DHT11 on PA1 ************/
static void DHT11_Pin_Output(void){
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}
static void DHT11_Pin_Input(void){
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}
static uint8_t DHT11_ReadByte(void){
  uint8_t i, b = 0;
  for(i = 0; i < 8; i++){
    uint32_t to=0;
    while(!HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1)){ if(++to>20000){ break; } }
    DWT_Delay_us(40);
    if(HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1)) b |= (1 << (7 - i));
    to=0;
    while(HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1)){ if(++to>20000){ break; } }
  }
  return b;
}
static int DHT11_Read_TempHum(int* tC, int* hPct){
  uint8_t rh_int, rh_dec, t_int, t_dec, checksum;

  DHT11_Pin_Output();
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_RESET);
  DWT_Delay_us(18000);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
  DWT_Delay_us(30);
  DHT11_Pin_Input();

  uint32_t timeout = 0;
  while(HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1)){ if(++timeout>30000) return 0; }
  timeout = 0;
  while(!HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1)){ if(++timeout>30000) return 0; }
  timeout = 0;
  while(HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1)){ if(++timeout>30000) return 0; }

  rh_int = DHT11_ReadByte();
  rh_dec = DHT11_ReadByte();
  t_int  = DHT11_ReadByte();
  t_dec  = DHT11_ReadByte();
  checksum = DHT11_ReadByte();

  if(((uint8_t)(rh_int + rh_dec + t_int + t_dec)) != checksum) return 0;

  *hPct = rh_int;
  *tC   = t_int;
  return 1;
}

/************ OLED helper ************/
static void OLED_ShowLines(const char* a, const char* b, const char* c, const char* d){
    SSD1306_Fill(SSD1306_COLOR_BLACK);
    SSD1306_GotoXY(0, 0);   SSD1306_Puts((char*)a, &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_GotoXY(0, 12);  SSD1306_Puts((char*)b, &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_GotoXY(0, 24);  SSD1306_Puts((char*)c, &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_GotoXY(0, 36);  SSD1306_Puts((char*)d, &Font_7x10, SSD1306_COLOR_WHITE);
    SSD1306_UpdateScreen();
}

static sys_status_t EvaluateStatus(int tC, int h, int soil, int motionNow){
  int danger = 0, caution = 0;

  if (tC >= TEMP_HIGH_C) danger = 1;
  if (h <= HUM_LOW_PCT || h >= HUM_HIGH_PCT) caution = 1;
  if (soil <= SOIL_DRY_PCT) caution = 1;
  if (motionNow) caution = 1;

  if (danger) return SYS_DANGER;
  if (caution) return SYS_CAUTION;
  return SYS_SAFE;
}

static void DriveOutputs(sys_status_t st) {
    if (st == SYS_SAFE) {
        LED_Green(1);
        LED_Yellow(0);
        LED_Red(0);
        Buzzer(0);
    }
    else if (st == SYS_CAUTION) {
        LED_Green(0);
        LED_Yellow(1);
        LED_Red(0);
        Buzzer(0);
    }
    else if (st == SYS_DANGER) {
        LED_Green(0);
        LED_Yellow(0);
        LED_Red(1);
        Buzzer(1);
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
  // DWT for microsecond delays (DHT11)
  DWT_Delay_Init();

  // Init OLED (change to ssd1306_Init(&hi2c1) if your lib needs the handle)
  SSD1306_Init();

  OLED_ShowLines("Mini Wx+Safety", "Booting...", "", "");
  HAL_Delay(800);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
 // --- Read Soil Sensor ---
    adc_raw = ADC_Read_PA0();
    soil_pct = Get_SoilPercent();

    // --- Read IR Sensor ---
    motion = IR_Read();

    // --- Read Temp & Humidity ---
    int dht_ok = DHT11_Read_TempHum((int*)&temp_c, (int*)&hum_pct);
    if (!dht_ok) {
      OLED_ShowLines("DHT11 error", "Retrying...", "", "");
      HAL_Delay(500);
      dht_ok = DHT11_Read_TempHum((int*)&temp_c, (int*)&hum_pct);
    }

    // --- Evaluate and drive outputs ---
    sys_status_t st = EvaluateStatus(temp_c, hum_pct, soil_pct, motion);
    DriveOutputs(st);

    // --- Prepare OLED Lines ---
    snprintf(l1, sizeof(l1), "T:%dC  H:%d%%", temp_c, hum_pct);
    snprintf(l2, sizeof(l2), "Soil:%d%% IR:%s", soil_pct, motion ? "YES":"NO");

    if (st == SYS_SAFE) {
      snprintf(l3, sizeof(l3), "Status: SAFE");
      snprintf(l4, sizeof(l4), soil_pct <= SOIL_DRY_PCT ?
              "Soil Dry! Water" : "Soil Normal, No Need");
    }
    else if (st == SYS_CAUTION) {
      snprintf(l3, sizeof(l3), "Status: CAUTION");
      if (soil_pct <= SOIL_DRY_PCT)
        snprintf(l4, sizeof(l4), "Soil Dry! Water");
      else if (hum_pct <= HUM_LOW_PCT || hum_pct >= HUM_HIGH_PCT)
        snprintf(l4, sizeof(l4), "Humidity alert");
      else if (motion)
        snprintf(l4, sizeof(l4), "Motion detected");
      else
        snprintf(l4, sizeof(l4), "Check env");
    }
    else { // SYS_DANGER
      snprintf(l3, sizeof(l3), "Status: DANGER");
      snprintf(l4, sizeof(l4), temp_c >= TEMP_HIGH_C ?
              "High Temp!" : "Critical");
    }

    // --- Update OLED ---
    OLED_ShowLines(l1, l2, l3, l4);
    HAL_Delay(500);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 400000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1|GPIO_PIN_10, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);

  /*Configure GPIO pin : PC13 */
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : PA1 */
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PB0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : PB1 PB10 */
  GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_10;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : PA8 */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
    // Blink RED LED fast if something fatal happened
    LED_Red(1);
    HAL_Delay(100);
    LED_Red(0);
    HAL_Delay(100);
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  (void)file; (void)line;
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */