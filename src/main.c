/*
 * Copyright (c) 2016 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 #include <zephyr/kernel.h>
 #include <zephyr/sys/printk.h>
 #include <zephyr/usb/usb_device.h>
 #include <zephyr/usb/usbd.h>
 #include <zephyr/drivers/uart.h>
 #include <zephyr/shell/shell.h>
 #include <string.h>
 
 BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart),
			  "Console device is not ACM CDC UART device");
 
 const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(uart1)); // Use UART1 for communication
 
 #define MAX_BUFFER_SIZE 256
 #define MSG_SIZE 128
 #define TERMINATING_CHAR '\r'
 
 static K_SEM_DEFINE(tx_done_sem, 0, 1);  // Semaphore for TX completion
 static volatile bool tx_busy = false;
 
 /* Buffers for communication */
 static char string_buffer[MAX_BUFFER_SIZE];
 static char rx_buf[MSG_SIZE];
 static int rx_buf_pos;
 
 /* UART callback function */
 static void uart_cb(const struct device *dev, void *user_data)
 {
	 uint8_t c;
 
	 if (!uart_irq_update(dev)) {
		 return;
	 }
 
	 /* Handle received data */
	 while (uart_irq_rx_ready(dev) && uart_fifo_read(dev, &c, 1) == 1) {
		 if ((c == '\n' || c == '\r') && rx_buf_pos > 0) {
			 rx_buf[rx_buf_pos] = '\0';
			 printk("Received: %s\n", rx_buf);
			 rx_buf_pos = 0;
		 } else if (rx_buf_pos < (sizeof(rx_buf) - 1)) {
			 rx_buf[rx_buf_pos++] = c;
		 }
	 }
 
	 /* Handle transmit complete */
	 if (uart_irq_tx_ready(dev)) {
		 tx_busy = false;
		 k_sem_give(&tx_done_sem);
		 uart_irq_tx_disable(dev);  // Disable TX interrupt until next transmission
	 }
 }
 
 /* Function to send string via UART with confirmation */
 static int uart_send_string(const char *str, size_t len)
 {
	 if (!device_is_ready(uart)) {
		 return -ENODEV;
	 }
 
	 /* Wait if there's an ongoing transmission */
	 while (tx_busy) {
		 k_sleep(K_MSEC(1));
	 }
 
	 tx_busy = true;
	 uart_irq_tx_enable(uart);
 
	 for (size_t i = 0; i < len; i++) {
		 uart_poll_out(uart, str[i]);
		 k_busy_wait(100);  // Small delay between characters
	 }
 
	 /* Wait for transmission to complete */
	 return k_sem_take(&tx_done_sem, K_MSEC(100));
 }
 
 /* Shell command to send string */
 static int cmd_send_string(const struct shell *shell, size_t argc, char **argv)
 {
	 if (argc != 2) {
		 shell_error(shell, "Usage: custom send \"your string\"");
		 return -EINVAL;
	 }
 
	 /* Clear buffer */
	 memset(string_buffer, 0, MAX_BUFFER_SIZE);
	 size_t input_len = strlen(argv[1]);
 
	 if (input_len < MAX_BUFFER_SIZE - 2) {  // -2 for \r and \0
		 /* Copy input string and add terminators */
		 strncpy(string_buffer, argv[1], input_len);
		 string_buffer[input_len] = TERMINATING_CHAR;
		 string_buffer[input_len + 1] = '\n';  // Add newline for better readability
 
		 /* Send via UART */
		 int ret = uart_send_string(string_buffer, input_len + 2);
		 
		 if (ret == 0) {
			 shell_print(shell, "Sent successfully: %s", argv[1]);
		 } else {
			 shell_error(shell, "Failed to send (error: %d)", ret);
		 }
		 
		 return ret;
	 }
 
	 shell_error(shell, "Input too long (max %d chars)", MAX_BUFFER_SIZE - 2);
	 return -EINVAL;
 }
 
 /* Shell command to read last received message */
 static int cmd_read_last(const struct shell *shell, size_t argc, char **argv)
 {
	 shell_print(shell, "Last received message: %s", rx_buf);
	 return 0;
 }
 
 /* Create command handlers */
 SHELL_STATIC_SUBCMD_SET_CREATE(sub_custom,
	 SHELL_CMD(send, NULL, "Send string over UART", cmd_send_string),
	 SHELL_CMD(read, NULL, "Read last received message", cmd_read_last),
	 SHELL_SUBCMD_SET_END
 );
 
 SHELL_CMD_REGISTER(custom, &sub_custom, "UART commands", NULL);
 
 int main(void)
 {
	 const struct device *const dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	 uint32_t dtr = 0;
 
	 if (!device_is_ready(uart)) {
		 printk("UART device not ready\n");
		 return -1;
	 }
 
 #if defined(CONFIG_USB_DEVICE_STACK_NEXT)
	 if (enable_usb_device_next()) {
		 return 0;
	 }
 #else
	 if (usb_enable(NULL)) {
		 return 0;
	 }
 #endif
 
	 /* Wait for DTR */
	 while (!dtr) {
		 uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
		 k_sleep(K_MSEC(100));
	 }
 
	 /* Setup UART */
	 uart_irq_callback_user_data_set(uart, uart_cb, NULL);
	 uart_irq_rx_enable(uart);
 
	 printk("UART Communication Ready\n");
 
	 /* Send startup message */
	 const char *init_msg = "UART initialized and ready for communication!\r\n";
	 uart_send_string(init_msg, strlen(init_msg));
 
	 while (1) {
		 k_sleep(K_MSEC(100));
	 }
 
	 return 0;
 }