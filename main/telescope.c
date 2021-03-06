#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "util.h"
#include "ssd1306.h"
#include "fonts.h"

#include "astro.h"
#include "mount_encoder.h"

#include "protocol.h"
#include "slew.h"

const static char *TAG = "Telescope";

/* ------ consts ---------- */
#define RA_CYCLE_MAX 30
#define RA_CYCLE_MIN 0.01
#define DEC_CYCLE_MAX 30
#define DEC_CYCLE_MIN 0.01
#define RA_SPEED_MAX 450000
#define RA_SPEED_MIN 150
#define DEC_SPEED_MAX 450000
#define DEC_SPEED_MIN 150
// #define RA_BACKLASH_TICKS 23
// #define DEC_BACKLASH_TICKS 21
// #define RA_TICKS_PER_CYCLE 312000
// #define DEC_TICKS_PER_CYCLE 156000

/* ------ utils ----------- */
#define DUTY_RES LEDC_TIMER_13_BIT
#define DUTY (((1 << DUTY_RES) - 1) / 2)
// #define LOGI(tag, format, ...)
// #define LOGE(tag, format, ...)
/* ------ configs ---------- */
#define WIFI_SSID   CONFIG_WIFI_SSID
#define WIFI_PASS   CONFIG_WIFI_PASS

#define UDP_PORT CONFIG_SERVER_PORT

#define DISPLAY_SCL (CONFIG_DISPLAY_SCL)
#define DISPLAY_SDA (CONFIG_DISPLAY_SDA)

#define GPIO_RA_EN  (CONFIG_GPIO_RA_EN)
#define GPIO_RA_DIR (CONFIG_GPIO_RA_DIR)
#define GPIO_RA_PUL (CONFIG_GPIO_RA_PUL)

#define RA_GEAR_RATIO ((double)(CONFIG_RA_GEAR_RATIO))
#define RA_RESOLUTION ((double)(CONFIG_RA_RESOLUTION))
#define RA_CYCLE_STEPS ((double)(CONFIG_RA_CYCLE_STEPS))

#define GPIO_DEC_EN  (CONFIG_GPIO_DEC_EN)
#define GPIO_DEC_DIR (CONFIG_GPIO_DEC_DIR)
#define GPIO_DEC_PUL (CONFIG_GPIO_DEC_PUL)

#define DEC_GEAR_RATIO ((double)(CONFIG_DEC_GEAR_RATIO))
#define DEC_RESOLUTION ((double)(CONFIG_DEC_RESOLUTION))
#define DEC_CYCLE_STEPS ((double)(CONFIG_DEC_CYCLE_STEPS))

#ifndef CONFIG_RA_REVERSE
#define CONFIG_RA_REVERSE false
#endif

#ifndef CONFIG_DEC_REVERSE
#define CONFIG_DEC_REVERSE false
#endif

/* ---------- FREQS ---------- */
#define RA_FREQ(cyclesPerSiderealDay) ((int)(RA_CYCLE_STEPS * RA_GEAR_RATIO * RA_RESOLUTION * (cyclesPerSiderealDay) * 1000 / SIDEREAL_DAY_MILLIS))
#define DEC_FREQ(cyclesPerDay) ((int)(DEC_CYCLE_STEPS * DEC_GEAR_RATIO * DEC_RESOLUTION * (cyclesPerDay) * 1000 / DAY_MILLIS))

#define CMD_PING 0
#define CMD_SET_TRACKING 1
#define CMD_SET_RA_SPEED 2
#define CMD_SET_DEC_SPEED 3
#define CMD_PULSE_GUIDING 4
#define CMD_SET_RA_GUIDE_SPEED 5
#define CMD_SET_DEC_GUIDE_SPEED 6
#define CMD_SYNC_TO_TARGET 7
#define CMD_SLEW_TO_TARGET 8
#define CMD_ABORT_SLEW 9
#define CMD_SET_SIDE_OF_PIER 10

#define PULSE_GUIDING_NONE 0
#define PULSE_GUIDING_DIR_WEST 4
#define PULSE_GUIDING_DIR_EAST 3
#define PULSE_GUIDING_DIR_NORTH 1
#define PULSE_GUIDING_DIR_SOUTH 2

ledc_channel_config_t ra_pmw_channel = {
    .channel = LEDC_CHANNEL_0,
    .timer_sel = LEDC_TIMER_0,
    .duty = 0,
    .gpio_num = GPIO_RA_PUL,
    .speed_mode = LEDC_HIGH_SPEED_MODE,
};

ledc_timer_config_t ra_pmw_timer = {
    .freq_hz = RA_FREQ(1),
    .duty_resolution = DUTY_RES,
    .speed_mode = LEDC_HIGH_SPEED_MODE,
    .timer_num = LEDC_TIMER_0
};

ledc_channel_config_t dec_pmw_channel = {
    .channel = LEDC_CHANNEL_1,
    .timer_sel = LEDC_TIMER_1,
    .duty = 0,
    .gpio_num = GPIO_DEC_PUL,
    .speed_mode = LEDC_HIGH_SPEED_MODE,
};

ledc_timer_config_t dec_pmw_timer = {
    .freq_hz = DEC_FREQ(1),
    .duty_resolution = DUTY_RES,
    .speed_mode = LEDC_HIGH_SPEED_MODE,
    .timer_num = LEDC_TIMER_1
};

typedef struct {
    char * title;
    char * line1;
    char * line2;
    char * line3;
    char line_font;
    char title_font;
} display_t;

bool displayEnabled = false;
void updateDisplay(display_t *content) {
    if (!displayEnabled) return;
    ssd1306_clear(0);    
    ssd1306_select_font(0, content->title_font ? content->title_font - 1 : 1);
    if (content->title) ssd1306_draw_string(0, 1, 3, content->title, 1, 0);
    ssd1306_select_font(0, content->line_font ? content->line_font - 1 : 1);
    if (content->line1) ssd1306_draw_string(0, 1, 19, content->line1, 1, 0);
    if (content->line2) ssd1306_draw_string(0, 1, 35, content->line2, 1, 0);
    if (content->line3) ssd1306_draw_string(0, 1, 51, content->line3, 1, 0);
    // ssd1306_draw_rectangle(0, 0, 0, 128, 16, 1);
	// ssd1306_draw_rectangle(0, 0, 16, 128, 48, 1);
    ssd1306_refresh(0, true);
}
int8_t tracking = 0;
char pulseGuiding = 0;
int raSpeed = 0, decSpeed = 0, raGuideSpeed = 7500, decGuideSpeed = 7500;
uint8_t sideOfPier = 0;

char my_ip[] = "255.255.255.255";
uint32_t my_ip_num;
char my_ip_port[] = "255.255.255.255:12345";
uint16_t my_ip_port_num;
char stepper_line1[] = "R.A. +00.0000 r/d";
char stepper_line2[] = "Dec  +00.0000 r/d";
char stepper_line3[] = "GUIDING/N  TRACKING/N";
display_t stepper_display = {
    .title = my_ip_port,
    .line1 = stepper_line1,
    .line2 = stepper_line2,
    .line3 = stepper_line3,
    .line_font = 1
};

void updateStepper() {
    double raCyclesPerSiderealDay = raSpeed / 15000.0;
    double decCyclesPerDay = decSpeed / 15000.0;

    char* guidingstr = "   ";
    switch (pulseGuiding) {
        case PULSE_GUIDING_DIR_NORTH:
            guidingstr = "G/N";
            decCyclesPerDay += decGuideSpeed / 15000.0;
            break;
        case PULSE_GUIDING_DIR_SOUTH:
            guidingstr = "G/S";
            decCyclesPerDay -= decGuideSpeed / 15000.0;
            break;
        case PULSE_GUIDING_DIR_WEST:
            guidingstr = "G/W";
            raCyclesPerSiderealDay += raGuideSpeed / 15000.0;
            break;
        case PULSE_GUIDING_DIR_EAST:
            guidingstr = "G/E";
            raCyclesPerSiderealDay -= raGuideSpeed / 15000.0;
            break;
    }

    if (tracking) {
        raCyclesPerSiderealDay += 1;
    }

    sprintf(stepper_line1, "R.A. %+8.4f r/d", raCyclesPerSiderealDay);
    sprintf(stepper_line2, "Dec  %+8.4f r/d", decCyclesPerDay);
    char* trackingstr = "   ";
    if (tracking > 0) {
        trackingstr = "T/N";
    } else if (tracking < 0) {
        trackingstr = "T/W";
    }
    
    if (!is_slewing()) {
        sprintf(stepper_line3, "%s               %s",guidingstr, trackingstr);
    } else {
        sprintf(stepper_line3, "                     ");
        int progress = (int)(get_slew_progress() * 100.0);
        int timeToGo = get_slew_time_to_go_millis() / 1000;
        sprintf(stepper_line3, "Slew %d%% eta %02d:%02d", progress, timeToGo / 60, timeToGo % 60);
    }
    updateDisplay(&stepper_display);

    if (raCyclesPerSiderealDay < 0) {
        raCyclesPerSiderealDay = -raCyclesPerSiderealDay;
        if (CONFIG_RA_REVERSE) {
            gpio_set_level(GPIO_RA_DIR, 1);
        } else {
            gpio_set_level(GPIO_RA_DIR, 0);
        }        
    } else {
        if (CONFIG_RA_REVERSE) {
            gpio_set_level(GPIO_RA_DIR, 0);
        } else {
            gpio_set_level(GPIO_RA_DIR, 1);
        }
    }

    int rafreq = RA_FREQ(raCyclesPerSiderealDay);
    if (raCyclesPerSiderealDay < RA_CYCLE_MIN || rafreq == 0) {
        LOGI(TAG, "RA Stop");
        ledc_set_duty(LEDC_HIGH_SPEED_MODE, ra_pmw_channel.channel, 0);
        ledc_update_duty(LEDC_HIGH_SPEED_MODE, ra_pmw_channel.channel);
        gpio_set_level(GPIO_RA_EN, 1);
    } else {
        if (raCyclesPerSiderealDay > RA_CYCLE_MAX) raCyclesPerSiderealDay = RA_CYCLE_MAX;        
        LOGI(TAG, "RA Freq: %d", rafreq);
        ledc_set_freq(LEDC_HIGH_SPEED_MODE, ra_pmw_timer.timer_num, rafreq);
        ledc_set_duty(LEDC_HIGH_SPEED_MODE, ra_pmw_channel.channel, DUTY);        
        ledc_update_duty(LEDC_HIGH_SPEED_MODE, ra_pmw_channel.channel);
        gpio_set_level(GPIO_RA_EN, 0);
    }

    if (decCyclesPerDay < 0) {
        decCyclesPerDay = -decCyclesPerDay;
        if (CONFIG_DEC_REVERSE) {
            gpio_set_level(GPIO_DEC_DIR, 1);
        } else {
            gpio_set_level(GPIO_DEC_DIR, 0);
        } 
    } else {
        if (CONFIG_DEC_REVERSE) {
            gpio_set_level(GPIO_DEC_DIR, 0);
        } else {
            gpio_set_level(GPIO_DEC_DIR, 1);
        } 
    }


    int decfreq = DEC_FREQ(decCyclesPerDay);
    if (decCyclesPerDay < DEC_CYCLE_MIN || decfreq == 0) {
        LOGI(TAG, "DEC Stop");
        ledc_set_duty(LEDC_HIGH_SPEED_MODE, dec_pmw_channel.channel, 0);
        ledc_update_duty(LEDC_HIGH_SPEED_MODE, dec_pmw_channel.channel);
        gpio_set_level(GPIO_DEC_EN, 1);
    } else {
        if (decCyclesPerDay > DEC_CYCLE_MAX) decCyclesPerDay = DEC_CYCLE_MAX;        
        LOGI(TAG, "DEC Freq: %d", decfreq);
        ledc_set_freq(LEDC_HIGH_SPEED_MODE, dec_pmw_timer.timer_num, decfreq);
        ledc_set_duty(LEDC_HIGH_SPEED_MODE, dec_pmw_channel.channel, DUTY);        
        ledc_update_duty(LEDC_HIGH_SPEED_MODE, dec_pmw_channel.channel);
        gpio_set_level(GPIO_DEC_EN, 0);
    }
}

void slewCallback(double raCyclesPerSiderealDay, double decCyclesPerDay) {
    raSpeed = raCyclesPerSiderealDay * 15000.0;
    decSpeed = decCyclesPerDay * 15000.0;
    updateStepper();
}

char ackBuf[18];
int8_t* ackTracking = (int8_t*)ackBuf;
char* ackPulseGuiding = ackBuf + 1;
int* ackRaSpeed = (int*)(ackBuf + 2);
int* ackDecSpeed = (int*)(ackBuf + 6);
int* ackRaGuideSpeed = (int*)(ackBuf + 10);
int* ackDecGuideSpeed = (int*)(ackBuf + 14);

void sendAck(int sock, struct sockaddr_in *addr, socklen_t addrlen) {    
    *ackTracking = tracking;
    *ackPulseGuiding = pulseGuiding;
    *ackRaSpeed = ntohl(raSpeed);
    *ackDecSpeed = ntohl(decSpeed);
    *ackRaGuideSpeed = ntohl(raGuideSpeed);
    *ackDecGuideSpeed = ntohl(decGuideSpeed);
    LOGI(TAG, "ack to %s:%d", inet_ntoa(addr->sin_addr), addr->sin_port);
    sendto(sock, ackBuf, LEN(ackBuf), 0, (struct sockaddr *) addr, addrlen);    
}

esp_timer_handle_t pulseGuidingTimer;
struct sockaddr_in lastPulseGuidingFrom;
socklen_t lastPulseGuidingFromLen;
int lastPulseGuidingSocket;

void pulseGuidingFinished(void* args) {
    pulseGuiding = PULSE_GUIDING_NONE;
    updateStepper();
    LOGI(TAG, "pulseGuide finished");
    sendAck(lastPulseGuidingSocket, &lastPulseGuidingFrom, lastPulseGuidingFromLen);
}

const char* getPulseDirDescr(int dir){
    switch (dir) {
        case PULSE_GUIDING_DIR_WEST:
        return "west";
        case PULSE_GUIDING_DIR_EAST:
        return "east";
        case PULSE_GUIDING_DIR_NORTH:
        return "north";
        case PULSE_GUIDING_DIR_SOUTH:
        return "south";
        default:
        return "unknown";
    }
}

int parse_command(char* buf, unsigned int len, int fromSocket, struct sockaddr_in* from, socklen_t fromlen) {
    char* cmd = buf;
    switch(*cmd) {
        case CMD_PING: {
            if (len != 1) return 0;
            LOGI(TAG, "ping");
        } break;
        case CMD_SET_TRACKING: {
            if (len != 2) return 0;
            if (is_slewing()) return 0;
            int8_t* newTracking = (int8_t*)(buf + 1);
            tracking = *newTracking;
            updateStepper();
            LOGI(TAG, "setTracking: %s", tracking ? (tracking > 0 ? "YES/N" : "YES/S") : "NO");
        } break;
        case CMD_SET_RA_SPEED: {
            if (len != 5) return 0;
            if (is_slewing()) return 0;
            int* newRaSpeed = (int*)(buf + 1);
            raSpeed = ntohl(*newRaSpeed);
            if (raSpeed > RA_SPEED_MAX) raSpeed = RA_SPEED_MAX;
            else if (raSpeed > RA_SPEED_MIN);
            else if (raSpeed > -RA_SPEED_MIN) raSpeed = 0;
            else if (raSpeed > -RA_SPEED_MAX);
            else raSpeed = -RA_SPEED_MAX;
            updateStepper();
            LOGI(TAG, "setRaSpeed: %f", raSpeed / 1000.0);
        } break;
        case CMD_SET_DEC_SPEED: {
            if (len != 5) return 0;
            if (is_slewing()) return 0;
            int* newDecSpeed = (int*)(buf + 1);
            decSpeed = ntohl(*newDecSpeed);
            if (decSpeed > DEC_SPEED_MAX) decSpeed = DEC_SPEED_MAX;
            else if (decSpeed > DEC_SPEED_MIN);
            else if (decSpeed > -DEC_SPEED_MIN) decSpeed = 0;
            else if (decSpeed > -DEC_SPEED_MAX);
            else decSpeed = -DEC_SPEED_MAX;
            updateStepper();
            LOGI(TAG, "setDecSpeed: %f", decSpeed / 1000.0);
        } break;
        case CMD_PULSE_GUIDING: {
            if (len != 4) return 0;
            if (is_slewing()) return 0;
            if (pulseGuiding) return 0;
            char* dir = (char*)(buf + 1);
            short* pulseLengthN = (short*)(buf + 2);
            short pulseLength = htons(*pulseLengthN);
            pulseGuiding = *dir;
            updateStepper();
            lastPulseGuidingFromLen = fromlen;
            memcpy(&lastPulseGuidingFrom, from, fromlen);
            lastPulseGuidingSocket = fromSocket;
            esp_timer_start_once(pulseGuidingTimer, pulseLength * 1000);
            LOGI(TAG, "pulseGuide: %s in %dms", getPulseDirDescr(*dir), pulseLength);
        } break;
        case CMD_SET_RA_GUIDE_SPEED: {
            if (len != 5) return 0;
            int* newRaGuideSpeed = (int*)(buf + 1);
            raGuideSpeed = ntohl(*newRaGuideSpeed);
            if (raGuideSpeed > RA_SPEED_MAX) raGuideSpeed = RA_SPEED_MAX;
            else if (raGuideSpeed > RA_SPEED_MIN);
            else if (raGuideSpeed > -RA_SPEED_MIN) raGuideSpeed = 0;
            else if (raGuideSpeed > -RA_SPEED_MAX);
            else raGuideSpeed = -RA_SPEED_MAX;
            updateStepper();
            LOGI(TAG, "setRaGuideSpeed: %f", raSpeed / 1000.0);
        } break;
        case CMD_SET_DEC_GUIDE_SPEED: {
            if (len != 5) return 0;
            int* newDecGuideSpeed = (int*)(buf + 1);
            decGuideSpeed = ntohl(*newDecGuideSpeed);
            if (decGuideSpeed > DEC_SPEED_MAX) decGuideSpeed = DEC_SPEED_MAX;
            else if (decGuideSpeed > DEC_SPEED_MIN);
            else if (decGuideSpeed > -DEC_SPEED_MIN) decGuideSpeed = 0;
            else if (decGuideSpeed > -DEC_SPEED_MAX);
            else decGuideSpeed = -DEC_SPEED_MAX;
            updateStepper();
            LOGI(TAG, "setDecGuideSpeed: %f", decGuideSpeed / 1000.0);
        } break;
        case CMD_SYNC_TO_TARGET: {
            if (len != 9) return 0;
            if (is_slewing()) return 0;
            int* raMillisPtr = (int*)(buf + 1);
            int* decMillisPtr = (int*)(buf + 5);
            int raMillis = ntohl(*raMillisPtr);
            int decMillis = ntohl(*decMillisPtr);
            set_angles(raMillis, decMillis);
            LOGI(TAG, "syncTo: %d, %d", raMillis, decMillis);
        }break;
        case CMD_SLEW_TO_TARGET: {
            if (is_slewing()) return 0;
            if (pulseGuiding) return 0;
            int* raMillisPtr = (int*)(buf + 1);
            int* decMillisPtr = (int*)(buf + 5);
            int raMillis = ntohl(*raMillisPtr);
            int decMillis = ntohl(*decMillisPtr);
            slew_to_coordinates(raMillis, decMillis);
            LOGI(TAG, "slewTo: %d, %d", raMillis, decMillis);
        }break;
        case CMD_ABORT_SLEW: {
            if (!is_slewing()) return 0;
            abort_slew();
            LOGI(TAG, "abortSlew");
        }break;
        case CMD_SET_SIDE_OF_PIER: {
            if (len != 2) return 0;
            if (is_slewing()) return 0;
            int8_t* newSideOfPier = (int8_t*)(buf + 1);
            sideOfPier = *newSideOfPier;
            int32_t ra = get_ra_angle_millis();
            int32_t dec = get_dec_angle_millis();
            set_angles(ra, dec);
            LOGI(TAG, "setSideOfPier: %s", sideOfPier ? "BeyondThePole/West" : "Normal/East");
        }break;
        default:
        LOGI(TAG, "Unknown command: %d", *buf);
        return 0;
        break;
    }
    return 1;
}



void udp_server(void *pvParameter) {

    LOGI(TAG, "Server Started");

    //bind loop
    while (1) {
        struct sockaddr_in saddr = { 0 };
        int sock = -1;
        int err = 0;
        
        sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            LOGE(TAG, "Failed to create socket. Error %d", errno);
            LOGE(TAG, "Retry After 1 second");
            SLEEP(1000);
            continue;
        }

        saddr.sin_family = PF_INET;
        saddr.sin_port = htons(UDP_PORT);
        saddr.sin_addr.s_addr = htonl(INADDR_ANY);
        err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
        if (err < 0) {
            LOGE(TAG, "Failed to bind socket. Error %d", errno);
            LOGE(TAG, "Retry After 1 second");
            SLEEP(1000);
            continue;
        }

        LOGI(TAG, "Server started at %d", UDP_PORT);

        updateStepper();

        //recv loop
        while (1) {
            char buf[129];
            struct sockaddr_in from;
            socklen_t fromlen;
            int count = recvfrom(sock, buf, 128, 0, (struct sockaddr *) &from, &fromlen);
            if (count <= 0) {
                continue;
            }
            parse_command(buf, count, sock, &from, fromlen);
            sendAck(sock, &from, fromlen);
        }
    }
}

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;
esp_err_t err;
bool connected = false;

void stepper_gpio_init(){
    gpio_pad_select_gpio(GPIO_RA_DIR);
    gpio_set_direction(GPIO_RA_DIR, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_RA_DIR, 1);

    gpio_pad_select_gpio(GPIO_RA_EN);
    gpio_set_direction(GPIO_RA_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_RA_EN, 1);

    gpio_pad_select_gpio(GPIO_DEC_DIR);
    gpio_set_direction(GPIO_DEC_DIR, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_DEC_DIR, 1);

    gpio_pad_select_gpio(GPIO_DEC_EN);
    gpio_set_direction(GPIO_DEC_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_DEC_EN, 1);
}

esp_timer_handle_t autoDiscoverTimer;

#define brdcPorts (CONFIG_SERVER_BROADCAST_PORT_LENGTH)
int brdcFd = -1;
struct sockaddr_in theirAddr[brdcPorts];

void autoDiscoverTick(void* args) {
    if (brdcFd == -1) {
        brdcFd = socket(PF_INET, SOCK_DGRAM, 0);
            if (brdcFd == -1) {
            LOGE(TAG, "autoDiscover socket fail: %d", errno);
            SLEEP(1000);
            esp_restart();
        }
        int optval = 1;//这个值一定要设置，否则可能导致sendto()失败  
        setsockopt(brdcFd, SOL_SOCKET, SO_BROADCAST | SO_REUSEADDR, &optval, sizeof(int));
        for (int i = 0; i < brdcPorts; i ++) {
            memset(&theirAddr[i], 0, sizeof(struct sockaddr_in));  
            theirAddr[i].sin_family = AF_INET;
            theirAddr[i].sin_addr.s_addr = inet_addr("255.255.255.255");  
            theirAddr[i].sin_port = htons(CONFIG_SERVER_BROADCAST_PORT_START + i);                  
        }
    }
    
    broadcast_t data;
    set_broadcast_fields(&data, 
        my_ip_num,
        UDP_PORT,
        get_ra_angle_millis(),
        get_dec_angle_millis(),
        is_slewing(),
        tracking,
        raSpeed,
        decSpeed,
        sideOfPier
    );
    
    for (int i = 0; i < brdcPorts; i ++) {
        sendto(brdcFd, data.buffer, BROADCAST_SIZE, 0, (struct sockaddr *)&(theirAddr[i]), sizeof(struct sockaddr));
    }    
}

static void wait_wifi(void *p)
{
    while (1) {
        LOGI(TAG, "Waiting for AP connection...");
        int dots = 0;
        EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, 1000 / portTICK_PERIOD_MS);
        while(!bits) {
            dots = (dots + 1) % 4;
            char searching[20] = "Searching WiFi \0\0\0\0";
            for (int i = 0; i < dots; i ++) {
                searching[i + (15/* "Searching WiFi ".length */)] = '.';
            }
            display_t disp = {
                .title = searching,
                .line1 = "WiFi config:",
                .line2 = "SSID: "WIFI_SSID,
                .line3 = "PASS: "WIFI_PASS,
            };
            updateDisplay(&disp);
            bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, 1000 / portTICK_PERIOD_MS);
        }
        LOGI(TAG, "Connected to AP");

        connected = true;

        SLEEP(1000);

        esp_timer_create_args_t args = {
            .dispatch_method = ESP_TIMER_TASK,
            .callback = pulseGuidingFinished
        };
        if (esp_timer_create(&args, &pulseGuidingTimer) != ESP_OK) {
            LOGI(TAG, "Failed to create pulse guiding timers");
            SLEEP(1000);
            esp_restart();
        }

        esp_timer_create_args_t argsAutoDiscover = {
            .dispatch_method = ESP_TIMER_TASK,
            .callback = autoDiscoverTick
        };
        if (esp_timer_create(&argsAutoDiscover, &autoDiscoverTimer) != ESP_OK) {
            LOGI(TAG, "Failed to create auto discover timers");
            SLEEP(1000);
            esp_restart();
        }
        esp_timer_start_periodic(autoDiscoverTimer, 1000 * 1000);
        
        ledc_channel_config(&ra_pmw_channel);
        ledc_timer_config(&ra_pmw_timer);

        ledc_channel_config(&dec_pmw_channel);
        ledc_timer_config(&dec_pmw_timer);

        udp_server(NULL);
    }

    vTaskDelete(NULL);
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        sprintf(my_ip, "%s", inet_ntoa(event->event_info.got_ip.ip_info.ip));
        my_ip_num = ntohl(event->event_info.got_ip.ip_info.ip.addr);
        sprintf(my_ip_port, "%s:%d", my_ip, UDP_PORT);
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        if (!connected) {
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        } else {
            for (int i = 3; i > 0; i --) {
                char buf[20];
                sprintf(buf, "Restart in %d sec", i);
                LOGE(TAG, "WiFi disconnected, %s", buf);
                display_t disp = {
                    .title = "WiFi Disconnected",
                    .line1 = buf,
                    .line2 = "",
                    .line3 = "",
                };
                updateDisplay(&disp);
                SLEEP(1000);
            }
            esp_restart();        
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void wifi_conn_init(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(wifi_event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

void app_main(void)
{
    LOGI("BOOT", "App main");
    LOGI("BOOT", "esp_timer_init");
    ESP_ERROR_CHECK_ALLOW_INVALID_STATE(esp_timer_init());
    LOGI("BOOT", "stepper_gpio_init");
    stepper_gpio_init();    
    LOGI("BOOT", "nvs_flash_init");
    ESP_ERROR_CHECK(nvs_flash_init());
    LOGI("BOOT", "init_mount");
    init_mount();
    LOGI("BOOT", "init_slew");
    init_slew(slewCallback);
    LOGI("BOOT", "ssd1306_init");
    if (ssd1306_init(0, CONFIG_DISPLAY_SCL, CONFIG_DISPLAY_SDA)) {
        LOGI(TAG, "Display inited");
        displayEnabled = true;
    } else {
        LOGE(TAG, "Cannot init display, try again in 1 sec");
        SLEEP(1000);
        if (ssd1306_init(0, CONFIG_DISPLAY_SCL, CONFIG_DISPLAY_SDA)) {
            LOGI(TAG, "Display inited");
            displayEnabled = true;
        } else {
            LOGE(TAG, "Cannot init display");
            displayEnabled = false;
        }
    }
    display_t disp = {
        .title = "Searching WiFi",
        .line1 = "WiFi config:",
        .line2 = "SSID: "WIFI_SSID,
        .line3 = "PASS: "WIFI_PASS,
    };
    LOGI("BOOT", "updateDisplay");
    updateDisplay(&disp);
    LOGI("BOOT", "wifi_conn_init");
    wifi_conn_init();
    LOGI("BOOT", "xTaskCreate wait_wifi");
    xTaskCreate(wait_wifi, TAG, 4096, NULL, 5, NULL);
}

uint8_t getSideOfPier() {
    return sideOfPier;
}


/* 90  - 21600000 */
/* 180 - 43200000 */
/* 270 - 64800000 */
/* 360 - 86400000 */
int32_t decMillis2decMecMillis(int32_t decMillis) {
    if (sideOfPier) {
        return 43200000 - decMillis;
    } else {
        return decMillis;
    }
}

int32_t decMecMillis2decMillis(int32_t decMecMillis, uint8_t* parseSideOfPier) {
    while (decMecMillis < 0) {
        decMecMillis += 86400000;
    }
    while (decMecMillis >= 86400000) {
        decMecMillis -= 86400000;
    }/* 0 - 360 */
    if (decMecMillis < 21600000) {
        if (parseSideOfPier) {
            *parseSideOfPier = 0;
        }    
        return decMecMillis;
    }
    if (decMecMillis > 64800000) {
        if (parseSideOfPier) {
            *parseSideOfPier = 0;
        }
        return decMecMillis - 86400000;
    }
    /* 90 - 270 */
    if (parseSideOfPier) {
        *parseSideOfPier = 1;
    }
    return 43200000 - decMecMillis;
}

void setSideOfPierWithDecMecMillis(int32_t decMecMillis) {
    decMecMillis2decMillis(decMecMillis, &sideOfPier);
}






