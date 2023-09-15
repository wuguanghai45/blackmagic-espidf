/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "general.h"
#include "gdb_if.h"
#include "version.h"

#include "gdb_packet.h"
#include "gdb_main.h"
#include "target.h"
#include "exception.h"
#include "gdb_packet.h"
#include "morse.h"
#include "platform.h"
#include "CBUF.h"

#include <assert.h>
#include <sys/time.h>
#include <sys/unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "dhcpserver/dhcpserver.h"
//#include <sysparam.h>
#include "http.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/api.h"
#include "lwip/tcp.h"

#include "rom/ets_sys.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp8266/uart_register.h"

#include <lwip/sockets.h>

#include "ota-tftp.h"

uint32_t target_clk_divider=0;
#define SWD_CYCLES_PER_CLOCK 19L
#define SWD_TOTAL_CYCLES 127L

bool debug_bmp;

void platform_max_frequency_set(uint32_t freq)
{
	if(freq < 50000) return;
  int cnt = (160000000L - SWD_TOTAL_CYCLES*(int)freq)/(SWD_CYCLES_PER_CLOCK*(int)freq);
  if(cnt < 0) cnt = 0;
  target_clk_divider = cnt;
  ESP_LOGI(__func__, "freq:%u set delay cycles: %d", freq, target_clk_divider);

}

uint32_t platform_max_frequency_get(void)
{
	return 160000000 / (target_clk_divider*SWD_CYCLES_PER_CLOCK+SWD_TOTAL_CYCLES);
}

nvs_handle h_nvs_conf;

uint32_t uart_overrun_cnt;
uint32_t uart_errors;
uint32_t uart_queue_full_cnt;
uint32_t uart_rx_count;
uint32_t uart_tx_count;

int tcp_serv_sock;
int udp_serv_sock;
int tcp_client_sock = 0;

static xQueueHandle uart_event_queue;
static struct sockaddr_in udp_peer_addr;

struct
{
  volatile uint8_t     m_get_idx;
  volatile uint8_t     m_put_idx;
  uint8_t              m_entry[ 256 ];
  SemaphoreHandle_t    sem;
} dbg_msg_queue;

#define IS_GPIO_USED(x) \
    CONFIG_TMS_SWDIO_GPIO==x || \
    CONFIG_TCK_SWCLK_GPIO==x || \
    CONFIG_TDI_GPIO==x || \
    CONFIG_TDO_GPIO==x || \
    CONFIG_SRST_GPIO==x
    
void platform_init() {
#ifdef USE_GPIO2_UART
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_UART1_TXD_BK);
#else
  PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
#endif

#if IS_GPIO_USED(1)
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1);
#endif

  gpio_clear(_, SWCLK_PIN);
  gpio_clear(_, SWDIO_PIN);

  gpio_enable(SWCLK_PIN, GPIO_OUTPUT);
  gpio_enable(SWDIO_PIN, GPIO_OUTPUT);

}

void platform_buffer_flush(void) {
  ;
}

void platform_srst_set_val(bool assert) {
  if(assert) {
    gpio_set_direction(CONFIG_SRST_GPIO, GPIO_OUTPUT);
    gpio_set_level(CONFIG_SRST_GPIO, 0);
  } else {
    gpio_set_level(CONFIG_SRST_GPIO, 1);
    gpio_set_direction(CONFIG_SRST_GPIO, GPIO_INPUT);
  }
}

bool platform_srst_get_val(void) {
  return !gpio_get_level(CONFIG_SRST_GPIO);
}

const char*
platform_target_voltage(void) {
  static char voltage[16];
  int vdd = esp_wifi_get_vdd33();
  sprintf(voltage, "%dmV", vdd);
  return voltage;
}

uint32_t platform_time_ms(void) {
  return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void platform_target_clk_output_enable(bool enable) {

}

void platform_nrst_set_val(bool assert) {

}

bool platform_nrst_get_val() {
  return 0;
}

#define vTaskDelayMs(ms)	vTaskDelay((ms)/portTICK_PERIOD_MS)

void platform_delay(uint32_t ms) {
  vTaskDelayMs(ms);
}

int platform_hwversion(void) {
  return 0;
}

void net_uart_task(void* params) {

	tcp_serv_sock = socket(AF_INET, SOCK_STREAM, 0);
	udp_serv_sock = socket(AF_INET, SOCK_DGRAM, 0);
	tcp_client_sock = 0;

	int ret;

	struct sockaddr_in saddr;
	saddr.sin_addr.s_addr = 0;
	saddr.sin_port = ntohs(23);
	saddr.sin_family = AF_INET;

	bind(tcp_serv_sock, (struct sockaddr*)&saddr, sizeof(saddr));

	saddr.sin_addr.s_addr = 0;
	saddr.sin_port = ntohs(2323);
	saddr.sin_family = AF_INET;
	bind(udp_serv_sock, (struct sockaddr*)&saddr, sizeof(saddr));
	listen(tcp_serv_sock, 1);

	while(1) {
		fd_set fds;
		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&fds);
		FD_SET(tcp_serv_sock, &fds);
		FD_SET(udp_serv_sock, &fds);
		if(tcp_client_sock)
			FD_SET(tcp_client_sock, &fds);

		int maxfd = MAX(tcp_serv_sock, MAX(udp_serv_sock, tcp_client_sock));

		if((ret = select(maxfd+1, &fds, NULL, NULL, &tv) > 0))
		{
			if(FD_ISSET(tcp_serv_sock, &fds)) {
				tcp_client_sock = accept(tcp_serv_sock, 0, 0);
				if(tcp_client_sock < 0) {
					ESP_LOGE(__func__, "accept() failed");
					tcp_client_sock = 0;
				} else {
					ESP_LOGI(__func__, "accepted tcp connection");

					int opt = 1; /* SO_KEEPALIVE */
			        setsockopt(tcp_client_sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&opt, sizeof(opt));
			        opt = 3; /* s TCP_KEEPIDLE */
			        setsockopt(tcp_client_sock, IPPROTO_TCP, TCP_KEEPIDLE, (void*)&opt, sizeof(opt));
			        opt = 1; /* s TCP_KEEPINTVL */
			        setsockopt(tcp_client_sock, IPPROTO_TCP, TCP_KEEPINTVL, (void *)&opt, sizeof(opt));
			        opt = 3; /* TCP_KEEPCNT */
			        setsockopt(tcp_client_sock, IPPROTO_TCP, TCP_KEEPCNT, (void *)&opt, sizeof(opt));
			        opt = 1;
			        setsockopt(tcp_client_sock, IPPROTO_TCP, TCP_NODELAY, (void *)&opt, sizeof(opt));

				}
			}
			uint8_t buf[128];

			if(FD_ISSET(udp_serv_sock, &fds)) {
				socklen_t slen = sizeof(udp_peer_addr);
				ret = recvfrom(udp_serv_sock, buf, sizeof(buf), 0, (struct sockaddr*)&udp_peer_addr, &slen);
				if(ret > 0) {
					uart_write_bytes(0, (const char*)buf, ret);
					uart_tx_count += ret;
				} else {
					ESP_LOGE(__func__, "udp recvfrom() failed");
				}
			}

			if(tcp_client_sock && FD_ISSET(tcp_client_sock, &fds)) {
				ret = recv(tcp_client_sock, buf, sizeof(buf), MSG_DONTWAIT);
				if(ret > 0) {
					uart_write_bytes(0, (const char*)buf, ret);
					uart_tx_count += ret;
				} else {
					ESP_LOGE(__func__, "tcp client recv() failed (%s)", strerror(errno));
					close(tcp_client_sock);
					tcp_client_sock = 0;
				}
			}
		}
	}

}

void uart_rx_task(void *parameters) {
  static uint8_t buf[TCP_MSS];
  int bufpos = 0;
  int ret;

  while (1) {
    uart_event_t evt;

    if (xQueueReceive(uart_event_queue, (void*)&evt, 100)) {

      if(evt.type == UART_FIFO_OVF) {
        uart_overrun_cnt++;
      }
      if(evt.type == UART_FRAME_ERR) {
        uart_errors++;
      }
      if(evt.type == UART_BUFFER_FULL) {
        uart_queue_full_cnt++;
      }

      bufpos = uart_read_bytes(0, &buf[bufpos], sizeof(buf)-bufpos, 0);

      //DEBUG("uart rx:%d\n", bufpos);
      if(bufpos > 0) {
        uart_rx_count += bufpos;
        http_term_broadcast_data(buf, bufpos);

        if (tcp_client_sock) {
        	ret = send(tcp_client_sock, buf, bufpos, 0);
        	if(ret < 0) {
        		ESP_LOGE(__func__, "tcp send() failed (%s)", strerror(errno));
        		close(tcp_client_sock);
        		tcp_client_sock = 0;
        	}
        }

        if(udp_peer_addr.sin_addr.s_addr) {
        	ret = sendto(udp_serv_sock, buf, bufpos, MSG_DONTWAIT, (struct sockaddr*)&udp_peer_addr, sizeof(udp_peer_addr));
        	if(ret < 0) {
        		ESP_LOGE(__func__, "udp send() failed (%s)", strerror(errno));
        		udp_peer_addr.sin_addr.s_addr = 0;
        	}
        }

        bufpos = 0;
      } //if(bufpos)
    }
  }
}



void dbg_task(void *parameters) {
	//struct netconn* nc = netconn_new(NETCONN_UDP);
	//ip_addr_t ip;
	//IP_ADDR4(&ip, 192, 168, 4, 255);

	while(1) {
    xSemaphoreTake(dbg_msg_queue.sem, -1);

    while(!CBUF_IsEmpty(dbg_msg_queue)) {
      char c = CBUF_Pop(dbg_msg_queue);
      http_debug_putc(c, c=='\n' ? 1 : 0);
    }
	}
}

void debug_putc(char c, int flush) {
  CBUF_Push(dbg_msg_queue, c);
  if(flush) {
    xSemaphoreGive(dbg_msg_queue.sem);
  }
}

void platform_set_baud(uint32_t baud) {
	uart_set_baudrate(0, baud);
	uart_set_baudrate(1, baud);
	nvs_set_u32(h_nvs_conf, "uartbaud", baud);
}

bool cmd_setbaud(target_s *t, int argc, const char **argv) {

	if (argc == 1) {
		uint32_t baud;
		uart_get_baudrate(0, &baud);
		gdb_outf("Current baud: %d\n", baud);
	}
	  if (argc == 2) {
		int baud = atoi(argv[1]);
		gdb_outf("Setting baud: %d\n", baud);

		platform_set_baud(baud);
	  }

  return 1;
}

void wifi_init_softap();

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    /* For accessing reason codes in case of disconnection */
    system_event_info_t *info = &event->event_info;

    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N ));

        break;
    case SYSTEM_EVENT_STA_CONNECTED:
        ESP_LOGI("WIFI", "connected:%s",
                 event->event_info.connected.ssid);
#ifdef CONFIG_BLACKMAGIC_HOSTNAME
        tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, CONFIG_BLACKMAGIC_HOSTNAME);

        ESP_LOGI("WIFI", "setting hostname:%s",
                  CONFIG_BLACKMAGIC_HOSTNAME);
#endif
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI("WIFI", "got ip:%s",
                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
        //xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_AP_STACONNECTED:
        ESP_LOGI("WIFI", "station:"MACSTR" join, AID=%d",
                 MAC2STR(event->event_info.sta_connected.mac),
                 event->event_info.sta_connected.aid);
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        ESP_LOGI("WIFI", "station:"MACSTR" leave, AID=%d",
                 MAC2STR(event->event_info.sta_disconnected.mac),
                 event->event_info.sta_disconnected.aid);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        ESP_LOGE("WIFI", "Disconnect reason : %d", info->disconnected.reason);
        if (info->disconnected.reason == WIFI_REASON_BASIC_RATE_NOT_SUPPORT) {
            /*Switch to 802.11 bgn mode */
            //esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCAL_11B | WIFI_PROTOCAL_11G | WIFI_PROTOCAL_11N);
        } else
        if(info->disconnected.reason == WIFI_REASON_NO_AP_FOUND) {
	        esp_wifi_disconnect();
	        wifi_init_softap();
        } else {
          esp_wifi_connect();
        }
        
        //xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}


void wifi_init_softap()
{
    //wifi_event_group = xEventGroupCreate();

    uint8_t val = 0;
    tcpip_adapter_dhcps_option(TCPIP_ADAPTER_OP_SET, TCPIP_ADAPTER_ROUTER_SOLICITATION_ADDRESS, &val, sizeof(dhcps_offer_t));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .ap = {
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .max_connection = CONFIG_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
			.channel = 11

        },
    };

    uint64_t chipid;
    esp_read_mac((uint8_t*)&chipid, ESP_MAC_WIFI_SOFTAP);
    
    if(strcmp(CONFIG_ESP_WIFI_SSID, "auto") == 0 || CONFIG_ESP_WIFI_IS_STATION) {
        wifi_config.ap.ssid_len = sprintf((char*)wifi_config.ap.ssid, "blackmagic_%X", (uint32_t)chipid);
    } else {
        wifi_config.ap.ssid_len = sprintf((char*)wifi_config.ap.ssid, CONFIG_ESP_WIFI_SSID);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N );

    ESP_ERROR_CHECK(esp_wifi_start());

}


void wifi_init_sta()
{
    ESP_LOGI(__func__, "wifi_init_sta begun.");

    uint8_t mac_address[8];
    esp_err_t result;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD
        },
    };

    result = esp_base_mac_addr_get(mac_address);

    if(result == ESP_ERR_INVALID_MAC) {
      ESP_LOGE(__func__, "base mac address invalid.  reading from fuse.");
      ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac_address));
      ESP_ERROR_CHECK(esp_base_mac_addr_set(mac_address));
      ESP_LOGE(__func__, "base mac address configured.");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );

    ESP_ERROR_CHECK(esp_wifi_start() );


    ESP_LOGI(__func__, "wifi_init_sta finished.");

}

//wifi_softap_set_dhcps_offer_option(OFFER_ROUTER, 0)
void uart_send_break() {
  uart_wait_tx_done(0, 10);

  uint32_t baud;
  uart_get_baudrate(0, &baud); //save current baudrate
  uart_set_baudrate(0, baud/2); // set half the baudrate
  char b = 0x00;
  uart_write_bytes(0, &b, 1);
  uart_wait_tx_done(0, 10);
  uart_set_baudrate(0, baud); //restore baudrate
}

//defined in blackmagic component
extern void main();

int putc_noop(int c) {
	return c;
}

int putc_remote(int c) {
	if(c == '\n')
		debug_putc('\r', 0);

	debug_putc(c, c=='\n' ? 1 : 0);
	return c;
}

extern void gdb_net_task();

void app_main(void) {
#if CONFIG_TARGET_UART
#warning Target UART configured.  ESP8266 debugging info will not be available via UART.
  ESP_LOGI(__func__, "deactivating debug uart");
  esp_log_set_putchar(putc_noop);
#endif

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(nvs_open("config", NVS_READWRITE, &h_nvs_conf));
  
  tcpip_adapter_init();
  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
  
#if CONFIG_ESP_WIFI_IS_STATION
  wifi_init_sta();
#else
  wifi_init_softap();

#endif

  httpd_start();

  esp_wifi_set_ps (WIFI_PS_NONE);

  CBUF_Init(dbg_msg_queue);
  dbg_msg_queue.sem = xSemaphoreCreateBinary();

#if CONFIG_TARGET_UART
  ESP_LOGI(__func__, "configuring uart for target");

  esp_log_set_putchar(putc_remote);

  uint32_t baud = 115200;
  nvs_get_u32(h_nvs_conf, "uartbaud", &baud);

  uart_set_baudrate(0, baud);
  uart_set_baudrate(1, baud);

  ESP_ERROR_CHECK(uart_driver_install(0, 4096, 256, 16, &uart_event_queue, 0));

  uart_intr_config_t uart_intr = {
      .intr_enable_mask = UART_RXFIFO_FULL_INT_ENA_M
      | UART_RXFIFO_TOUT_INT_ENA_M
      | UART_FRM_ERR_INT_ENA_M
      | UART_RXFIFO_OVF_INT_ENA_M,
      .rxfifo_full_thresh = 80,
      .rx_timeout_thresh = 2,
      .txfifo_empty_intr_thresh = 10
  };

  uart_intr_config(0, &uart_intr);

#endif

  platform_init();

  xTaskCreate(&dbg_task, "dbg_main", 4096, NULL, 4, NULL);
  xTaskCreate(&gdb_net_task, "gdb_net", 4096, NULL, 2, NULL);

#if CONFIG_TARGET_UART
  xTaskCreate(&uart_rx_task, "uart_rx_task", 1200, NULL, 5, NULL);
  xTaskCreate(&net_uart_task, "net_uart_task", 1200, NULL, 5, NULL);
#endif

  ota_tftp_init_server(69, 4);

  ESP_LOGI(__func__, "Free heap %d\n", esp_get_free_heap_size());
 
}

#ifndef ENABLE_DEBUG
__attribute((used))
int ets_printf(const char *__restrict c, ...) { return 0; }

__attribute((used))
int printf(const char *__restrict c, ...) { return 0; }
#endif

bool platform_spi_init(const spi_bus_e bus)
{
	(void)bus;
	return false;
}

bool platform_spi_deinit(const spi_bus_e bus)
{
	(void)bus;
	return false;
}

bool platform_spi_chip_select(const uint8_t device_select)
{
	(void)device_select;
	return false;
}

uint8_t platform_spi_xfer(const spi_bus_e bus, const uint8_t value)
{
	(void)bus;
	return value;
}
