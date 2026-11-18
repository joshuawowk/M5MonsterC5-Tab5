import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SETTING = "CONFIG_IN_TRANSFER_BUFFER_SIZE"
CH34X_BULK_MPS = 32


def config_value(path: Path, setting: str):
    match = re.search(
        rf"^{re.escape(setting)}=(\d+)$",
        path.read_text(encoding="utf-8"),
        flags=re.MULTILINE,
    )
    return int(match.group(1)) if match else None


class UsbCdcTransferConfigTest(unittest.TestCase):
    def test_active_config_completes_one_full_ch34x_packet(self):
        self.assertEqual(
            config_value(ROOT / "sdkconfig", SETTING),
            CH34X_BULK_MPS,
        )

    def test_clean_build_keeps_the_ch34x_packet_sized_transfer(self):
        self.assertEqual(
            config_value(ROOT / "sdkconfig.defaults", SETTING),
            CH34X_BULK_MPS,
        )

    def test_driver_exposes_bidirectional_transfer_diagnostics(self):
        header = (
            ROOT
            / "managed_components/espressif__iot_usbh_cdc/include/iot_usbh_cdc.h"
        ).read_text(encoding="utf-8")
        source = (
            ROOT / "managed_components/espressif__iot_usbh_cdc/iot_usbh_cdc.c"
        ).read_text(encoding="utf-8")

        self.assertIn("usbh_cdc_debug_stats_t", header)
        self.assertIn("usbh_cdc_get_debug_stats", header)
        for field in (
            "rx_buffered",
            "tx_buffered",
            "rx_completed",
            "tx_completed",
            "rx_bytes",
            "tx_bytes",
            "rx_errors",
            "tx_errors",
            "rx_submit_errors",
            "tx_submit_errors",
            "last_rx_status",
            "last_tx_status",
            "last_rx_submit_error",
            "last_tx_submit_error",
        ):
            self.assertIn(field, header)
        self.assertIn("usbh_cdc_get_debug_stats", source)

    def test_driver_rounds_the_in_transfer_up_to_the_endpoint_packet_size(self):
        # A CH343/CH9102 bridge has a 64-byte BULK IN endpoint. A 32-byte IN
        # transfer there is refused by the host stack and the first submit
        # aborts the app as soon as the bridge connects.
        source = (
            ROOT / "managed_components/espressif__iot_usbh_cdc/iot_usbh_cdc.c"
        ).read_text(encoding="utf-8")
        alloc = source[source.index("size_t in_buf_len = CONFIG_IN_TRANSFER_BUFFER_SIZE;"):]
        alloc = alloc[:alloc.index("usb_host_transfer_alloc(in_buf_len")]
        self.assertIn("in_buf_len % in_mps != 0", alloc)
        self.assertIn("(in_buf_len + in_mps - 1) / in_mps * in_mps", alloc)

    def test_cdc_acm_bridge_gets_115200_line_coding(self):
        # A CH343 on the Monster's UART port comes up at 9600 baud (measured on
        # the unit): without SET_LINE_CODING JanOS never hears "ping".
        source = (ROOT / "main/main.c").read_text(encoding="utf-8")
        helper = source[source.index("static bool cdc_acm_init_port_115200(void)"):]
        helper = helper[:helper.index("static void usb_vcp_config_task")]
        self.assertIn("CDC_REQ_SET_LINE_CODING", helper)
        self.assertIn("const uint32_t baud = UART_BAUD_RATE;", helper)
        self.assertIn("usb_cdc_acm_needs_line_coding()", source)
        # Espressif USB-Serial-JTAG STALLs class requests: never send it them.
        self.assertIn("usb_last_vid != USB_ESPRESSIF_VID", source)
        self.assertIn("bInterfaceSubClass == 0x02", source)

    def test_application_keeps_driver_warnings_and_logs_timeout_snapshots(self):
        source = (ROOT / "main/main.c").read_text(encoding="utf-8")
        self.assertIn('esp_log_level_set("USBH_CDC", ESP_LOG_WARN)', source)
        self.assertIn('usb_log_cdc_state("ack_timeout")', source)
        self.assertIn("usbh_cdc_get_debug_stats", source)
        self.assertRegex(source, r"#define HS_CRACK_REMOTE_LINE_BYTES\s+640")


if __name__ == "__main__":
    unittest.main()
