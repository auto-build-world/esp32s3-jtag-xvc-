/*
   Description: Xilinx Virtual Cable Server for ESP32 (ESP-IDF)

   This work, "xvc-esp32", is a derivative of "xvcpi.c"
   (https://github.com/derekmulcahy/xvcpi) by Derek Mulcahy.

   "xvc-esp32" is licensed under CC0 1.0 Universal
   (http://creativecommons.org/publicdomain/zero/1.0/)
   by Kenta IDA (fuga@fugafuga.org)

   The original license information of "xvcpi.c" is attached below.

   This work, "xvcpi.c", is a derivative of "xvcServer.c"
   (https://github.com/Xilinx/XilinxVirtualCable) by Avnet and is used
   by Xilinx for XAPP1251.

   "xvcServer.c" is licensed under CC0 1.0 Universal
   (http://creativecommons.org/publicdomain/zero/1.0/)
   by Avnet and is used by Xilinx for XAPP1251.

   "xvcServer.c", is a derivative of "xvcd.c" (https://github.com/tmbinc/xvcd)
   by tmbinc, used under CC0 1.0 Universal
   (http://creativecommons.org/publicdomain/zero/1.0/).

   Portions of "xvcpi.c" are derived from OpenOCD (http://openocd.org)

   "xvcpi.c" is licensed under CC0 1.0 Universal
   (http://creativecommons.org/publicdomain/zero/1.0/)
   by Derek Mulcahy.
*/

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#ifdef USE_STATIC_IP
#include "lwip/ip4_addr.h"
#endif

#include "credentials.h"

// ---------------------------------------------------------------------------
// USB Serial/JTAG (USB XVC bridge mode)
// ---------------------------------------------------------------------------
#include "driver/usb_serial_jtag.h"

#define ERROR_JTAG_INIT_FAILED -1
#define ERROR_OK 1

// GPIO numbers for each signal.
// Only supports GPIOs below 32 (jtag_xfer uses uint32_t bit ops).
static constexpr const int tms_gpio = 11;
static constexpr const int tck_gpio = 10;
static constexpr const int tdo_gpio = 9;
static constexpr const int tdi_gpio = 8;

// Transition delay coefficients
static unsigned int jtag_delay = 100;

static constexpr const char* TAG = "XVC";

// ---------------------------------------------------------------------------
// JTAG primitives
// ---------------------------------------------------------------------------

static void jtag_write(std::uint_fast8_t tck, std::uint_fast8_t tms,
                        std::uint_fast8_t tdi)
{
  gpio_set_level(static_cast<gpio_num_t>(tck_gpio), tck);
  gpio_set_level(static_cast<gpio_num_t>(tms_gpio), tms);
  gpio_set_level(static_cast<gpio_num_t>(tdi_gpio), tdi);

  for (std::uint32_t i = 0; i < jtag_delay; i++)
    asm volatile ("nop");
}

static bool jtag_read(void)
{
  return gpio_get_level(static_cast<gpio_num_t>(tdo_gpio)) & 1;
}

static std::uint32_t jtag_xfer(std::uint_fast8_t n, std::uint32_t tms,
                                std::uint32_t tdi)
{
  std::uint32_t tdo = 0;
  for (int i = 0; i < n; i++) {
    jtag_write(0, tms & 1, tdi & 1);
    tdo |= jtag_read() << i;
    jtag_write(1, tms & 1, tdi & 1);
    tms >>= 1;
    tdi >>= 1;
  }
  return tdo;
}

static int jtag_init(void)
{
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;

  // TDO = input
  io_conf.pin_bit_mask = 1ULL << tdo_gpio;
  io_conf.mode = GPIO_MODE_INPUT;
  gpio_config(&io_conf);

  // TDI, TCK, TMS = outputs
  io_conf.pin_bit_mask =
    (1ULL << tdi_gpio) | (1ULL << tck_gpio) | (1ULL << tms_gpio);
  io_conf.mode = GPIO_MODE_OUTPUT;
  gpio_config(&io_conf);

  gpio_set_level(static_cast<gpio_num_t>(tdi_gpio), 0);
  gpio_set_level(static_cast<gpio_num_t>(tck_gpio), 0);
  gpio_set_level(static_cast<gpio_num_t>(tms_gpio), 1);

  return ERROR_OK;
}

// ---------------------------------------------------------------------------
// Transport-independent XVC protocol handler
// ---------------------------------------------------------------------------

// read_fn: read exactly len bytes into buf. Return 1 on success, <=0 on error.
// write_fn: write exactly len bytes from buf. Return 1 on success, <=0 on error.
typedef int (*xvc_read_t)(void* ctx, void* buf, size_t len);
typedef int (*xvc_write_t)(void* ctx, const void* buf, size_t len);

static bool xvc_handle_command(xvc_read_t read_fn, xvc_write_t write_fn,
                                void* ctx)
{
  std::uint8_t cmd[16];
  std::memset(cmd, 0, 16);

  if (read_fn(ctx, cmd, 2) != 1)
    return false;

  if (memcmp(cmd, "ge", 2) == 0) {
    // getinfo
    if (read_fn(ctx, cmd, 6) != 1)
      return false;
    const char xvcInfo[] = "xvcServer_v1.0:2048\n";
    return write_fn(ctx, xvcInfo, strlen(xvcInfo)) == 1;
  } else if (memcmp(cmd, "se", 2) == 0) {
    // settck
    if (read_fn(ctx, cmd, 9) != 1)
      return false;
    std::uint8_t result[4];
    memcpy(result, cmd + 5, 4);
    return write_fn(ctx, result, 4) == 1;
  } else if (memcmp(cmd, "sh", 2) == 0) {
    // shift
    if (read_fn(ctx, cmd, 4) != 1)
      return false;
  } else {
    return false;
  }

  // shift: read bit length
  int len;
  if (read_fn(ctx, &len, 4) != 1)
    return false;

  int nr_bytes = (len + 7) / 8;
  // Static buffers — too large for FreeRTOS task stack.
  // Single-threaded access (one XVC transport at a time), so this is safe.
  static std::uint8_t buffer[2048];
  if (nr_bytes * 2 > sizeof(buffer))
    return false;

  if (read_fn(ctx, buffer, nr_bytes * 2) != 1)
    return false;

  static std::uint8_t result[1024];
  std::memset(result, 0, nr_bytes);

  jtag_write(0, 1, 1);

  int bytesLeft = nr_bytes;
  int bitsLeft = len;
  int byteIndex = 0;
  uint32_t tdi = 0, tms = 0, tdo = 0;

  while (bytesLeft > 0) {
    tms = 0;
    tdi = 0;
    tdo = 0;
    if (bytesLeft >= 4) {
      memcpy(&tms, &buffer[byteIndex], 4);
      memcpy(&tdi, &buffer[byteIndex + nr_bytes], 4);
      tdo = jtag_xfer(32, tms, tdi);
      memcpy(&result[byteIndex], &tdo, 4);
      bytesLeft -= 4;
      bitsLeft -= 32;
      byteIndex += 4;
    } else {
      memcpy(&tms, &buffer[byteIndex], bytesLeft);
      memcpy(&tdi, &buffer[byteIndex + nr_bytes], bytesLeft);
      tdo = jtag_xfer(bitsLeft, tms, tdi);
      memcpy(&result[byteIndex], &tdo, bytesLeft);
      bytesLeft = 0;
      break;
    }
  }

  jtag_write(0, 1, 0);

  return write_fn(ctx, result, nr_bytes) == 1;
}

// ---------------------------------------------------------------------------
// Socket helpers
// ---------------------------------------------------------------------------

static int sread(int fd, void* target, int len)
{
  std::uint8_t *t = reinterpret_cast<std::uint8_t*>(target);
  while (len) {
    int r = read(fd, t, len);
    if (r <= 0)
      return r;
    t += r;
    len -= r;
  }
  return 1;
}

// Socket transport wrappers for xvc_handle_command
static int sock_read_fn(void* ctx, void* buf, size_t len)
{
  int fd = static_cast<int>(reinterpret_cast<intptr_t>(ctx));
  return sread(fd, buf, len);
}

static int sock_write_fn(void* ctx, const void* buf, size_t len)
{
  int fd = static_cast<int>(reinterpret_cast<intptr_t>(ctx));
  return write(fd, buf, len) == static_cast<int>(len) ? 1 : -1;
}

struct Socket
{
  int fd;
  Socket() : fd(-1) {}
  Socket(int fd) : fd(fd) {}
  Socket(int domain, int family, int protocol)
  {
    this->fd = socket(domain, family, protocol);
  }
  Socket(const Socket&) = delete;
  Socket(Socket&& rhs) : fd(rhs.fd)
  {
    rhs.fd = -1;
  }
  ~Socket()
  {
    this->release();
  }
  void release()
  {
    if (this->is_valid()) {
      close(this->fd);
      this->fd = -1;
    }
  }
  int get() const
  {
    return this->fd;
  }

  Socket& operator=(Socket&& rhs)
  {
    this->fd = rhs.fd;
    rhs.fd = -1;
    return *this;
  }
  bool is_valid() const
  {
    return this->fd > 0;
  }
  operator int() const
  {
    return this->get();
  }
};

// ---------------------------------------------------------------------------
// XVC Server
// ---------------------------------------------------------------------------

class XvcServer
{
  private:
    Socket listen_socket;
    Socket client_socket;
  public:
    XvcServer(std::uint16_t port)
    {
      Socket sock(AF_INET, SOCK_STREAM, 0);
      {
        int value = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
      }

      sockaddr_in address;
      address.sin_addr.s_addr = INADDR_ANY;
      address.sin_port = htons(port);
      address.sin_family = AF_INET;

      if (bind(sock, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket.");
      }
      if (listen(sock, 0) < 0) {
        ESP_LOGE(TAG, "Failed to listen the socket.");
      }

      ESP_LOGI(TAG, "Begin XVC Server. port=%d", port);
      this->listen_socket = std::move(sock);
    }
    XvcServer(const XvcServer&) = delete;

    bool wait_connection()
    {
      fd_set conn;
      int maxfd = this->listen_socket.get();

      FD_ZERO(&conn);
      FD_SET(this->listen_socket.get(), &conn);

      fd_set read = conn, except = conn;
      int fd;

      if (select(maxfd + 1, &read, 0, &except, 0) < 0) {
        ESP_LOGE(TAG, "select");
        return false;
      }

      for (fd = 0; fd <= maxfd; ++fd) {
        if (FD_ISSET(fd, &read)) {
          if (fd == this->listen_socket.get()) {
            int newfd;
            sockaddr_in address;
            socklen_t nsize = sizeof(address);
            newfd = accept(this->listen_socket.get(),
                           reinterpret_cast<sockaddr*>(&address), &nsize);

            ESP_LOGI(TAG, "connection accepted - fd %d\n", newfd);
            if (newfd < 0) {
              ESP_LOGE(TAG, "accept returned an error.");
            } else {
              if (newfd > maxfd) {
                maxfd = newfd;
              }
              FD_SET(newfd, &conn);
              this->client_socket = Socket(newfd);
              return true;
            }
          }
        }
      }
      return false;
    }

    bool handle_data()
    {
      int fd = this->client_socket.get();
      void* ctx = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
      return xvc_handle_command(sock_read_fn, sock_write_fn, ctx);
    }

    void run()
    {
      if (this->client_socket.is_valid()) {
        if (!this->handle_data()) {
          this->client_socket.release();
        }
      } else {
        if (this->wait_connection()) {
          // Nothing to do.
        }
      }
    }
};

// ---------------------------------------------------------------------------
// USB-JTAG bridge mode (XVC handled on PC side)
// ---------------------------------------------------------------------------
//
// ESP32 acts as a dumb USB Serial/JTAG to JTAG adapter. PC runs the XVC server.
// Binary protocol over USB CDC (native USB port):
//
//   Byte 0  Name  Description
//   ------  ----  -----------------------------------------------
//   'I'     Init  Init JTAG GPIOs (no response)
//   'S'     Shift n_bits(uint16 LE) + TMS[n/8 bytes] + TDI[n/8]
//                  Response: TDO[n/8 bytes]  (max n_bits = 1024)

// Read exactly len bytes from USB Serial/JTAG. Returns 0 on success, -1 on error.
static int usb_read_exact(std::uint8_t* buf, size_t len, TickType_t timeout)
{
  while (len > 0) {
    int r = usb_serial_jtag_read_bytes(buf, len, timeout);
    if (r < 0) return -1;
    if (r == 0) return -1;  // timeout
    buf += r;
    len -= static_cast<size_t>(r);
  }
  return 0;
}

// Write all bytes to USB Serial/JTAG. Returns 0 on success, -1 on error.
static int usb_write_all(const std::uint8_t* buf, size_t len, TickType_t timeout)
{
  while (len > 0) {
    int r = usb_serial_jtag_write_bytes(buf, len, timeout);
    if (r <= 0) return -1;
    buf += r;
    len -= static_cast<size_t>(r);
  }
  return 0;
}

// Forward declaration (watchdog creates this task)
static void jtag_bridge_task(void*);

// Forward declaration for wifi commands (defined later in this file)
static void handle_cmd_line(const char* line);

// USB bridge watchdog: in WiFi mode, periodically check for 'I' on USB
// and switch to USB bridge mode if detected.
// Also processes "wifi" console commands (WiFi mode only).
static void usb_watchdog_task(void*)
{
  std::uint8_t data;
  char cmd_buf[128];
  int cmd_pos = 0;
  TickType_t last_tick = 0;

  while (true) {
    int r = usb_serial_jtag_read_bytes(&data, 1, pdMS_TO_TICKS(10));
    if (r > 0) {
      char ch = static_cast<char>(data);
      last_tick = xTaskGetTickCount();

      // Check for bridge mode switch (high priority)
      if (cmd_pos == 0 && ch == 'I') {
        ESP_LOGI(TAG, "USB 'I' detected — switching to USB bridge mode");
        esp_wifi_stop();
        esp_wifi_deinit();
        std::uint8_t ack = 0x06;
        usb_serial_jtag_write_bytes(&ack, 1, pdMS_TO_TICKS(100));
        xTaskCreatePinnedToCore(jtag_bridge_task, "jtag_bridge", 16384,
                                nullptr, 1, nullptr, APP_CPU_NUM);
        vTaskDelete(nullptr);
      }

      // Echo to USB Serial/JTAG so user sees what they type in monitor
      usb_serial_jtag_write_bytes(&data, 1, pdMS_TO_TICKS(100));

      if (cmd_pos == 0 && ch == 'w') {
        cmd_buf[cmd_pos++] = ch;
      } else if (cmd_pos > 0) {
        if (ch == '\n' || ch == '\r') {
          cmd_buf[cmd_pos] = '\0';
          cmd_pos = 0;
          if (strncmp(cmd_buf, "wifi ", 5) == 0)
            handle_cmd_line(cmd_buf);
        } else if (cmd_pos < (int)sizeof(cmd_buf) - 1) {
          cmd_buf[cmd_pos++] = ch;
        }
      }
    } else if (cmd_pos > 0 && (xTaskGetTickCount() - last_tick) > pdMS_TO_TICKS(2000)) {
      // Idle 2 seconds — auto-process accumulated command
      cmd_buf[cmd_pos] = '\0';
      cmd_pos = 0;
      if (strncmp(cmd_buf, "wifi ", 5) == 0)
        handle_cmd_line(cmd_buf);
    }
  }
}

static void jtag_bridge_task(void*)
{
  ESP_LOGI(TAG, "USB-JTAG bridge task started");
  std::uint8_t cmd;
  while (true) {
    // Block until a command byte arrives
    if (usb_read_exact(&cmd, 1, portMAX_DELAY) != 0)
      continue;

    switch (cmd) {
    case 'I':
      jtag_init();
      // Send ACK so proxy knows we're ready and any stale boot data is flushed
      {
        std::uint8_t ack = 0x06;
        usb_write_all(&ack, 1, pdMS_TO_TICKS(100));
      }
      break;

    case 'S': {
      // Read bit count (uint16_t, little-endian)
      std::uint16_t n_bits;
      if (usb_read_exact(reinterpret_cast<std::uint8_t*>(&n_bits),
                         2, pdMS_TO_TICKS(100)) != 0)
        break;
      if (n_bits < 1 || n_bits > 16384) break;

      int nr_bytes = (n_bits + 7) / 8;

      // Read concatenated TMS + TDI data
      // Use heap allocation to avoid large stack array
      std::uint8_t* buf = new (std::nothrow) std::uint8_t[nr_bytes * 2 + 4];
      if (!buf) break;
      // Large shifts (thousands of bytes) need longer USB read timeout
      int read_timeout = (nr_bytes > 256) ? 2000 : 500;
      if (usb_read_exact(buf, nr_bytes * 2, pdMS_TO_TICKS(read_timeout)) != 0) {
        delete[] buf;
        break;
      }

      std::uint8_t* tms = buf;
      std::uint8_t* tdi = buf + nr_bytes;

      jtag_write(0, 1, 1);

      int remain_bytes = nr_bytes;
      int remain_bits = static_cast<int>(n_bits);
      int idx = 0;

      while (remain_bytes > 0) {
        std::uint32_t tms32 = 0, tdi32 = 0, tdo32 = 0;
        if (remain_bytes >= 4) {
          memcpy(&tms32, &tms[idx], 4);
          memcpy(&tdi32, &tdi[idx], 4);
          tdo32 = jtag_xfer(32, tms32, tdi32);
          memcpy(&buf[idx], &tdo32, 4);
          remain_bytes -= 4;
          remain_bits -= 32;
          idx += 4;
        } else {
          memcpy(&tms32, &tms[idx], remain_bytes);
          memcpy(&tdi32, &tdi[idx], remain_bytes);
          tdo32 = jtag_xfer(remain_bits, tms32, tdi32);
          memcpy(&buf[idx], &tdo32, remain_bytes);
          remain_bytes = 0;
        }
      }

      jtag_write(0, 1, 0);

      // Simple framing: 2-byte length (LE) + TDO
      std::uint8_t len_prefix[2];
      len_prefix[0] = static_cast<std::uint8_t>(nr_bytes & 0xff);
      len_prefix[1] = static_cast<std::uint8_t>((nr_bytes >> 8) & 0xff);
      usb_write_all(len_prefix, 2, pdMS_TO_TICKS(1000));
      usb_write_all(buf, nr_bytes, pdMS_TO_TICKS(3000));
      delete[] buf;
      break;
    }
    }
  }
}

// ---------------------------------------------------------------------------
// Serial bridge (USB UART ↔ FPGA UART)
// ---------------------------------------------------------------------------

static void serialTask(void*)
{
  std::uint8_t data;
  while (true) {
    if (uart_read_bytes(UART_NUM_0, &data, 1, pdMS_TO_TICKS(10)) > 0) {
      uart_write_bytes(UART_NUM_1, (const char*)&data, 1);
    }
    if (uart_read_bytes(UART_NUM_1, &data, 1, 0) > 0) {
      uart_write_bytes(UART_NUM_0, (const char*)&data, 1);
    }
  }
}

// ---------------------------------------------------------------------------
// UART init
// ---------------------------------------------------------------------------

static void uart_init(void)
{
  uart_config_t uart_config = {};
  uart_config.baud_rate = 115200;
  uart_config.data_bits = UART_DATA_8_BITS;
  uart_config.parity = UART_PARITY_DISABLE;
  uart_config.stop_bits = UART_STOP_BITS_1;
  uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uart_config.source_clk = UART_SCLK_DEFAULT;

  // UART_NUM_0 = USB serial (console)
  uart_param_config(UART_NUM_0, &uart_config);
  if (!uart_is_driver_installed(UART_NUM_0)) {
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
  }

  // UART_NUM_1 = FPGA UART (RX=33, TX=23)
  uart_param_config(UART_NUM_1, &uart_config);
  uart_set_pin(UART_NUM_1, 23, 33, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_driver_install(UART_NUM_1, 256, 0, 0, NULL, 0);
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

// ---------------------------------------------------------------------------
// NVS WiFi config (persistent SSID/passphrase)
// ---------------------------------------------------------------------------

// Write formatted string to console (USB Serial/JTAG or UART).
// In WiFi mode, USB Serial/JTAG is idle and safe to use.
// In bridge mode, this path is never hit (commands run in watchdog, WiFi-only).
static void console_printf(const char* fmt, ...)
{
  char buf[256];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n > 0) {
    int maxlen = (int)sizeof(buf) - 3;
    int len = (n < maxlen) ? n : maxlen;
    buf[len] = '\r';
    buf[len + 1] = '\n';
    usb_serial_jtag_write_bytes(buf, len + 2, pdMS_TO_TICKS(100));
  }
}

static constexpr const char* WIFI_NVS_NS = "wifi_cfg";

static bool nvs_wifi_load(char* ssid, size_t ssid_sz, char* pass, size_t pass_sz)
{
  nvs_handle_t h;
  if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
  bool ok = false;
  size_t len = ssid_sz;
  if (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK) {
    len = pass_sz;
    ok = (nvs_get_str(h, "pass", pass, &len) == ESP_OK);
  }
  nvs_close(h);
  return ok;
}

static void nvs_wifi_save(const char* ssid, const char* pass)
{
  nvs_handle_t h;
  if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, "ssid", ssid);
  nvs_set_str(h, "pass", pass);
  nvs_commit(h);
  nvs_close(h);
  console_printf("WiFi saved to NVS (SSID: %s)", ssid);
}

static void nvs_wifi_erase()
{
  nvs_handle_t h;
  if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_all(h);
  nvs_commit(h);
  nvs_close(h);
  console_printf("WiFi credentials erased from NVS");
}

// ---------------------------------------------------------------------------
// WiFi command handlers (called from serialTask when "wifi ..." detected)
// ---------------------------------------------------------------------------

static void wifi_reconnect_with(const char* ssid, const char* pass)
{
  wifi_config_t wifi_config = {};
  strcpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid);
  strcpy(reinterpret_cast<char*>(wifi_config.sta.password), pass);
  ESP_ERROR_CHECK(esp_wifi_disconnect());
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_connect());
}

// Handle a complete "wifi ..." command line
static void handle_cmd_line(const char* line)
{
  if (strncmp(line, "wifi set ", 9) == 0) {
    const char* rest = line + 9;
    const char* space = strchr(rest, ' ');
    if (space && (space - rest) < 32 && strlen(space + 1) < 64) {
      char ssid[32], pass[64];
      int slen = space - rest;
      memcpy(ssid, rest, slen);
      ssid[slen] = '\0';
      strcpy(pass, space + 1);
      nvs_wifi_save(ssid, pass);
      wifi_reconnect_with(ssid, pass);
    } else {
      console_printf("Usage: wifi set <ssid> <password>");
    }
  } else if (strcmp(line, "wifi show") == 0) {
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
      console_printf("WiFi SSID: %s", wc.sta.ssid);
    }
    char saved_ssid[32] = "", saved_pass[64] = "";
    if (nvs_wifi_load(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass))) {
      console_printf("NVS stored: SSID=%s (overrides default)", saved_ssid);
    } else {
      console_printf("NVS: using default from credentials.h");
    }
  } else if (strcmp(line, "wifi reset") == 0) {
    nvs_wifi_erase();
    wifi_reconnect_with(MY_SSID, MY_PASSPHRASE);
  } else if (strcmp(line, "wifi speed") == 0 || strcmp(line, "wifi delay") == 0) {
    unsigned int freq_khz = static_cast<unsigned int>(1000000.0f / (2.0f * (jtag_delay * 4.17f + 30.0f)) / 1000.0f);
    console_printf("JTAG delay=%d (~%u kHz TCK)", jtag_delay, freq_khz);
  } else if (strncmp(line, "wifi delay ", 11) == 0) {
    int val = atoi(line + 11);
    if (val >= 1 && val <= 1000) {
      jtag_delay = static_cast<unsigned int>(val);
      // Save to NVS
      nvs_handle_t h;
      if (nvs_open("jtag_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "delay", static_cast<std::uint32_t>(val));
        nvs_commit(h);
        nvs_close(h);
      }
      unsigned int freq_khz = static_cast<unsigned int>(1000000.0f / (2.0f * (val * 4.17f + 30.0f)) / 1000.0f);
      console_printf("JTAG delay set to %d (~%u kHz TCK)", val, freq_khz);
    } else {
      console_printf("Usage: wifi delay <1-1000>");
    }
  } else {
    console_printf("Unknown: %s", line);
    console_printf("Commands: wifi set <ssid> <pass>, wifi show, wifi reset, wifi delay <1-1000>, wifi speed");
  }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT
             && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGI(TAG, "WiFi disconnected. Reconnecting...");
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
    esp_wifi_connect();
  } else if (event_base == IP_EVENT
             && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* event =
      reinterpret_cast<ip_event_got_ip_t*>(event_data);
    ESP_LOGI(TAG, "WiFi connection ready. IP: " IPSTR,
             IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static void wifi_init_sta(void)
{
  wifi_event_group = xEventGroupCreate();

  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_t *netif = esp_netif_create_default_wifi_sta();

#ifdef USE_STATIC_IP
  esp_netif_dhcpc_stop(netif);
  esp_netif_ip_info_t ip_info;
  IP4_ADDR(reinterpret_cast<ip4_addr_t*>(&ip_info.ip), 192, 168, 1, 12);
  IP4_ADDR(reinterpret_cast<ip4_addr_t*>(&ip_info.gw), 192, 168, 1, 1);
  IP4_ADDR(reinterpret_cast<ip4_addr_t*>(&ip_info.netmask),
           255, 255, 255, 0);
  esp_netif_set_ip_info(netif, &ip_info);
#else
  (void)netif;
#endif

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_register(WIFI_EVENT,
                                       ESP_EVENT_ANY_ID,
                                       &wifi_event_handler,
                                       NULL, NULL);
  esp_event_handler_instance_register(IP_EVENT,
                                       IP_EVENT_STA_GOT_IP,
                                       &wifi_event_handler,
                                       NULL, NULL);

  wifi_config_t wifi_config = {};
  {
    char ssid[32], pass[64];
    strncpy(ssid, MY_SSID, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    strncpy(pass, MY_PASSPHRASE, sizeof(pass) - 1);
    pass[sizeof(pass) - 1] = '\0';
    if (nvs_wifi_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
      ESP_LOGI(TAG, "WiFi: using NVS-stored credentials (SSID=%s)", ssid);
    }
    strcpy(reinterpret_cast<char*>(wifi_config.sta.ssid), ssid);
    strcpy(reinterpret_cast<char*>(wifi_config.sta.password), pass);
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "WiFi STA initialized. Connecting to AP...");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

extern "C" void app_main()
{
  // Common init for both modes
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES
      || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Load JTAG delay from NVS
  {
    nvs_handle_t h;
    if (nvs_open("jtag_cfg", NVS_READONLY, &h) == ESP_OK) {
      std::uint32_t val = 0;
      if (nvs_get_u32(h, "delay", &val) == ESP_OK && val >= 1 && val <= 1000) {
        jtag_delay = val;
      }
      nvs_close(h);
    }
  }

  jtag_init();
  uart_init();

  // Block all output during USB detection to prevent stale-boot-data
  // corruption of the USB bridge protocol. Save default to restore in
  // WiFi mode so console (idf.py monitor) output still works.
  vprintf_like_t old_vprintf =
    esp_log_set_vprintf([](const char*, va_list) -> int { return 0; });

  // Install USB Serial/JTAG driver (safe even if no cable connected)
  usb_serial_jtag_driver_config_t usb_serial_jtag_cfg = {
    .tx_buffer_size = 2048,
    .rx_buffer_size = 2048,
  };
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_serial_jtag_cfg));

  // Start WiFi in background (non-blocking)
  wifi_init_sta();

  // Serial bridge (CH340 UART ↔ FPGA UART) — also handles "wifi" console commands
  xTaskCreatePinnedToCore(serialTask, "serial", 4096, nullptr,
                          1, nullptr, APP_CPU_NUM);

  // Auto-detect mode: wait 3 seconds for proxy's 'I' init command on USB
  std::uint8_t cmd;
  bool usb_detected = (usb_serial_jtag_read_bytes(&cmd, 1,
                         pdMS_TO_TICKS(3000)) > 0) && (cmd == 'I');

  if (usb_detected) {
    // USB-JTAG bridge mode
    ESP_LOGI(TAG, "USB proxy detected — entering USB-JTAG bridge mode");

    // Send ACK to proxy
    std::uint8_t ack = 0x06;
    usb_serial_jtag_write_bytes(&ack, 1, pdMS_TO_TICKS(100));

    // Stop WiFi to save power
    esp_wifi_stop();
    esp_wifi_deinit();

    // Start USB bridge task
    xTaskCreatePinnedToCore(jtag_bridge_task, "jtag_bridge", 16384,
                            nullptr, 1, nullptr, APP_CPU_NUM);

    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  } else {
    // WiFi TCP mode — restore console output first
    esp_log_set_vprintf(old_vprintf);
    ESP_LOGI(TAG, "No USB proxy detected — entering WiFi XVC server mode");

    // Start USB watchdog: continuously monitors USB for 'I' command,
    // switches to USB bridge mode if detected later.
    xTaskCreatePinnedToCore(usb_watchdog_task, "usb_watchdog", 2048,
                            nullptr, 1, nullptr, APP_CPU_NUM);

    std::unique_ptr<XvcServer> server;

    while (true) {
      if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) {
        if (!server) {
          ESP_LOGI(TAG, "WiFi connection ready.");
          server.reset(new XvcServer(2542));
        }
        server->run();
      } else {
        if (server) {
          ESP_LOGI(TAG, "disconnected from WiFi.");
          server.reset();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
  }
}
