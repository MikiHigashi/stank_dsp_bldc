// 自作ブラシレスドライバー dsPIC 版
// PWM1L PWM2L PWM3L は名称と異なり、ハイサイドをドライブしている
// LED1 オレンジ 電流超過状態で点灯
// LED2 イエロー 点滅＝センサー異常 点灯＝ストールと判断し遮断
#define FCY 69784687UL
#include <libpic30.h>
#include "mcc_generated_files/mcc.h"
#include <stdio.h>
#include <string.h>
#include "soft_i2c.h"
#include "lcd_i2c.h"


uint16_t table_pwm[] = {
	0,
	24,
	26,
	28,
	30,
	33,
	35,
	38,
	41,
	44,
	48,
	51,
	55,
	60,
	64,
	70,
	75,
	81,
	87,
	94,
	102,
	110,
	118,
	128,
	138,
	149,
	161,
	173,
	187,
	202,
	218,
	235,
	253,
	273,
	295,
	318,
	344,
	371,
	400,
	432,
	466,
	502,
	542,
	585,
	631,
	681,
	735,
	793,
	855,
	923,
	996,
	1070,
	1140,
	1210,
	1281,
	1351,
	1421,
	1491,
	1561,
	1631,
	1701,
	1771,
	1842,
	1912,
	1982,
	2052,
	2122,
	2192,
	2262,
	2332,
	2403,
	2473,
	2543,
	2613,
	2683,
	2753,
	2823,
	2894,
	2964,
	3034,
	3104,
	3174,
	3244,
	3314,
	3384,
	3455,
	3525,
	3595,
	3665,
	3735,
	3805,
	3875,
	3945,
	4016,
	4086,
	4156,
	4226,
	4296,
	4366,
	4436,
	4506,
	4577,
	4647,
	4717,
	4787,
	4857,
	4927,
	4997,
	5068,
	5138,
	5208,
	5278,
	5348,
	5418,
	5488,
	5558,
	5629,
	5699,
	5769,
	5839,
	5909,
	5979,
	6049,
	6119,
	6190,
	6260,
	6330,
	6400,
	6400
};



uint8_t right;    // スロットル

# define MAX_BUF_I 4 /* motor_i 過去履歴バッファ数 */
uint16_t motor_i; // 電流センサー値

#define MAX_CHANGE 5

// これ以下の DUTY にしない max 6400
#define MIN_DUTY 1000 
#define SMALL_DUTY 1000 
// pwm がこれ以上になると電子進角を増やす（２ステップ先に切りかえる）
#define ADVANCE_ANGLE 1500 
    
uint8_t acc, acc0 = 128;

uint8_t cw_ccw; // 回転方向 1:前進 0:停止 255:バック
uint16_t pwm; // 実際の PWM DUTY
uint16_t pwm_buf; // 反映待ち PWM DUTY
//uint16_t pwmi; // PWM DUTY の希望値
uint16_t ii, delay; // WAIT


// 相変化に要した時間
uint16_t countValLower = 0;
uint16_t countValUpper = 0;


// PWM の希望値をセット
void set_pwm(uint16_t p) {
//    if (motor_if >= MOTOR_I_HIGH) { // 予測電流超過
//    pwm_buf = p;
    pwm = p;
}


void to_0(void) {
    PDC3 = pwm;
    OUT1L_SetHigh();
    PDC1 = 0;
    PDC2 = 0;
    OUT2L_SetLow();
    OUT3L_SetLow();
}


void to_1(void) {
    PDC2 = pwm;
    OUT1L_SetHigh();
    PDC1 = 0;
    PDC3 = 0;
    OUT2L_SetLow();
    OUT3L_SetLow();
}


void to_2(void) {
    PDC2 = pwm;
    OUT3L_SetHigh();
    PDC1 = 0;
    PDC3 = 0;
    OUT1L_SetLow();
    OUT2L_SetLow();
}


void to_3(void) {
    PDC1 = pwm;
    OUT3L_SetHigh();
    PDC2 = 0;
    PDC3 = 0;
    OUT1L_SetLow();
    OUT2L_SetLow();
}


void to_4(void) {
    PDC1 = pwm;
    OUT2L_SetHigh();
    PDC2 = 0;
    PDC3 = 0;
    OUT1L_SetLow();
    OUT3L_SetLow();
}


void to_5(void) {
    PDC3 = pwm;
    OUT2L_SetHigh();
    PDC1 = 0;
    PDC2 = 0;
    OUT1L_SetLow();
    OUT3L_SetLow();
}


uint8_t table_phase[] = {
    0,
    4,
    2,
    3,
    0,
    5,
    1,
    0
};

// ホールセンサーを元に、現在の相を判定
uint8_t get_phase(void) {
    return table_phase[(PORTA >> 2) & 7];
}

   
void (*table_int_fw_full[])(void) = {
    to_0,
    to_5,
    to_3,
    to_4,
    to_1,
    to_0,
    to_2,
    to_0
};
void (*table_int_fw2_full[])(void) = {
    to_0,
    to_0,
    to_4,
    to_5,
    to_2,
    to_1,
    to_3,
    to_0
};
void (*table_int_bk_full[])(void) = {
    to_0,
    to_3,
    to_1,
    to_2,
    to_5,
    to_4,
    to_0,
    to_0
};
void (*table_int_bk2_full[])(void) = {
    to_0,
    to_2,
    to_0,
    to_1,
    to_4,
    to_3,
    to_5,
    to_0
};
void (*table_int_nt[])(void) = {
    to_0,
    to_4,
    to_2,
    to_3,
    to_0,
    to_5,
    to_1,
    to_0
};


// ウエイト用タイマー3
// 次の相に移行させる実
void int_timer3(void) {
    TMR3_Stop();

    if ((countValUpper | countValLower) == 0) {
        countValUpper = 65535;
    }
    
    if (cw_ccw == 1) { // 順回転
        //if (pwm < ADVANCE_ANGLE) {
        if (countValUpper > 1) {
            (table_int_fw_full[(PORTA >> 2) & 7])();
        }
        else {        
            (table_int_fw2_full[(PORTA >> 2) & 7])();
        }
        TMR2 = 0;
        return;
    }
    if (cw_ccw) { // 逆回転
        //if (pwm < ADVANCE_ANGLE) {
        if (countValUpper > 1) {
            (table_int_bk_full[(PORTA >> 2) & 7])();
        }
        else {        
            (table_int_bk2_full[(PORTA >> 2) & 7])();
        }
        TMR2 = 0;
        return;
    }
    (table_int_nt[(PORTA >> 2) & 7])();
    TMR2 = 0;
}


// ホールセンサーの状態変化割り込み
void int_pin(void) {
    countValLower = TMR4;
    countValUpper = TMR5HLD;
    TMR5HLD = 0;
    TMR4 = 0;
    WATCHDOG_TimerClear();
    TMR2 = 0;
    
    if (delay > 1) {
        TMR3_Period16BitSet(delay >> 1);
        TMR3_Counter16BitSet(0);
        TMR3_Start();
        return;
    }
    int_timer3();
}


// ホールセンサーを元に、次の相に移行
void to_next_phase(void) {
    if (cw_ccw == 1) { // 順回転
        (table_int_fw_full[(PORTA >> 2) & 7])();
        return;
    }
    if (cw_ccw) { // 逆回転
        (table_int_bk_full[(PORTA >> 2) & 7])();
        return;
    }
    (table_int_nt[(PORTA >> 2) & 7])();
}


// 脱調対策タイマー2
// 240ms ごと
// 相変化が240ms以上発生しないとタイマー割り込みが発生しここに来る
void int_timer(void) {
    if (acc != 128) {
        delay = 0;
        set_pwm(SMALL_DUTY);
        to_next_phase();
    }
    else {
            cw_ccw = pwm = 0;
            delay = 0;
            PDC1 = 0;                
            PDC2 = 0;                
            PDC3 = 0;
            OUT1L_SetLow();
            OUT2L_SetLow();
            OUT3L_SetLow();
    }
    TMR2 = 0;
}


// センサーチェック
int check_sensor(void) {
    uint8_t i;
    uint8_t ok = 0;

    for (i=0; i<8; i++) {
        if ((motor_i >= 508) && (motor_i <= 516)) {
            ok ++;
        }
    }

    if (ok < 7) {
        return 0; // センサー接続不良
    }
    return 1;
}


// センサーエラー表示
int show_sensor_error(void) {
    uint8_t i;
    char buf[20];
    
    for (i=0; i<4; i++) {
        WATCHDOG_TimerClear();

        LCD_i2C_cmd(0x80);
        sprintf(buf, "SENSOR =%4u", motor_i);
        LCD_i2C_data(buf);
        __delay_ms(100);
    }
}
   

int main(void)
 {
    unsigned long u;
    uint16_t pp;

    // initialize the device
    SYSTEM_Initialize();
    
    PDC1 = 0;                
    PDC2 = 0;                
    PDC3 = 0;
    OUT1L_SetLow();
    OUT2L_SetLow();
    OUT3L_SetLow();
    PWM_Enable();
    
    DMA_ChannelEnable(DMA_CHANNEL_0);
    DMA_ChannelEnable(DMA_CHANNEL_1);
    DMA_PeripheralAddressSet(DMA_CHANNEL_0, (volatile unsigned int) &SPI2BUF);
    DMA_PeripheralAddressSet(DMA_CHANNEL_1, (volatile unsigned int) &ADC1BUF0);
    DMA_StartAddressASet(DMA_CHANNEL_0, (uint16_t)(&right));        
    DMA_StartAddressASet(DMA_CHANNEL_1, (uint16_t)(&motor_i));        
    CN_SetInterruptHandler(int_pin);
    TMR2_SetInterruptHandler(int_timer);
    TMR3_SetInterruptHandler(int_timer3);
//    PWM_SetSpecialEventInterruptHandler(pwm_adc1);

    WATCHDOG_TimerClear();

    char buf[32];
    __delay_ms(100);    
    LCD_i2c_init(8);
    
    cw_ccw = pwm = delay = 0;
    acc = acc0 = 128;
    TMR2_Start();
    TMR3_Stop();
    TMR4_Start();
    WATCHDOG_TimerClear();

  uint16_t cnt1 = 50; // 電源投入直後の不安定をパス

    // センサー安定待ち
    while (check_sensor() == 0) {
        show_sensor_error();
    }
    
    while (1)
    {
        set_pwm(pwm); // 電流値だけチェック

        if (cnt1) {
            cnt1 --;
            acc = 128;
        }
        else {
            acc = right; // スロットル
        }
        
        // スロットル激変緩和
        if (acc > acc0) {
            if ((uint16_t)acc > ((uint16_t)acc0 + MAX_CHANGE)) {
                if (((uint16_t)acc0 + MAX_CHANGE) > 255) {
                    acc = 255;
                }
                else {
                    acc = (acc0 + MAX_CHANGE);
                }
            }
        }
        else {
            if (((uint16_t)acc + MAX_CHANGE) < (uint16_t)acc0) {
                if (acc0 > MAX_CHANGE) {
                    acc = (acc0 - MAX_CHANGE);
                }
                else {
                    acc = 0;
                }
            }            
        }
        
        if (acc > 128) {
            if (acc0 < 128) {
                acc = 128;
            }
        }
        else if (acc < 128) {
            if (acc0 > 128) {
                acc = 128;
            }
        }
            
        if (acc > 128) {
            cw_ccw = 1;
            pp = table_pwm[acc - 128];
        }
        else if (acc < 128) {
            cw_ccw = 255;
            pp = table_pwm[128 - acc];
        }

        if (acc == 128) {
            WATCHDOG_TimerClear();
            cw_ccw = pwm = 0;
            delay = 0;
            PDC1 = 0;                
            PDC2 = 0;                
            PDC3 = 0;
            OUT1L_SetLow();
            OUT2L_SetLow();
            OUT3L_SetLow();
            TMR2 = 0;
        }
        else {
            if (acc0 == 128) {
                delay = 0;
                set_pwm(SMALL_DUTY);
                to_next_phase();
                TMR2 = 0;
            }
            else if (pp < MIN_DUTY) {
                u = 1600;
                u *= (MIN_DUTY - pp);
                u /= pp;
                if (u > 65535) {
                    u = 65535;
                }
                delay = (uint16_t)u;
                set_pwm(MIN_DUTY);
            }
            else {
                set_pwm(pp);
                delay = 0;
            }
        }
        
//        LCD_i2C_cmd(0x80);
//        sprintf(buf, "%3u %3u", right, (PORTA >> 2) & 7);
//        sprintf(buf, "%4u %4u %5u", motor_i, motor_if, pwm_if);
//        LCD_i2C_data(buf);
//        __delay_ms(20);
         
        acc0 = acc;
    }
    return 1; 
}
/**
 End of File
*/

