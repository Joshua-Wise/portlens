#include "protocols/eapol_detect.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "eapol";

// EAPOL packet types (IEEE 802.1X-2004, Section 11.3)
#define EAPOL_TYPE_EAP_PACKET     0
#define EAPOL_TYPE_START          1
#define EAPOL_TYPE_LOGOFF         2
#define EAPOL_TYPE_KEY            3
#define EAPOL_TYPE_ASF_ALERT      4

// EAP codes (RFC 3748)
#define EAP_CODE_REQUEST          1
#define EAP_CODE_RESPONSE         2
#define EAP_CODE_SUCCESS          3
#define EAP_CODE_FAILURE          4

// EAP method types
#define EAP_TYPE_IDENTITY         1
#define EAP_TYPE_NAK              3
#define EAP_TYPE_MD5_CHALLENGE    4
#define EAP_TYPE_TLS             13
#define EAP_TYPE_TTLS            21
#define EAP_TYPE_PEAP            25

// EAPOL header: version(1) + type(1) + body_length(2) = 4 bytes
#define EAPOL_HDR_LEN  4
// EAP header: code(1) + id(1) + length(2) = 4 bytes
#define EAP_HDR_LEN    4

static bool    s_detected = false;
static char    s_status[64] = {0};

static const char *eap_type_str(uint8_t type)
{
    switch (type) {
    case EAP_TYPE_IDENTITY:      return "Identity";
    case EAP_TYPE_MD5_CHALLENGE: return "MD5";
    case EAP_TYPE_TLS:           return "EAP-TLS";
    case EAP_TYPE_TTLS:          return "EAP-TTLS";
    case EAP_TYPE_PEAP:          return "PEAP";
    default:                     return "Other";
    }
}

void eapol_process(const uint8_t *data, uint32_t length)
{
    // data starts after the Ethernet header (14 bytes already stripped)
    if (length < EAPOL_HDR_LEN) return;

    uint8_t eapol_version = data[0];
    uint8_t eapol_type    = data[1];
    uint16_t body_len     = (data[2] << 8) | data[3];

    ESP_LOGI(TAG, "EAPOL frame: ver=%u type=%u body_len=%u",
             eapol_version, eapol_type, body_len);

    s_detected = true;

    if (eapol_type == EAPOL_TYPE_EAP_PACKET && body_len >= EAP_HDR_LEN &&
        length >= EAPOL_HDR_LEN + EAP_HDR_LEN) {
        const uint8_t *eap = data + EAPOL_HDR_LEN;
        uint8_t eap_code = eap[0];

        if (eap_code == EAP_CODE_REQUEST && body_len >= EAP_HDR_LEN + 1) {
            uint8_t eap_method = eap[4];
            if (eap_method == EAP_TYPE_IDENTITY) {
                snprintf(s_status, sizeof(s_status), "802.1X (ID req)");
            } else {
                snprintf(s_status, sizeof(s_status), "802.1X (%s)", eap_type_str(eap_method));
            }
            ESP_LOGI(TAG, "EAP-Request method=%s", eap_type_str(eap_method));
        } else if (eap_code == EAP_CODE_SUCCESS) {
            snprintf(s_status, sizeof(s_status), "802.1X (open)");
        } else if (eap_code == EAP_CODE_FAILURE) {
            snprintf(s_status, sizeof(s_status), "802.1X (denied)");
        } else {
            snprintf(s_status, sizeof(s_status), "802.1X active");
        }
    } else {
        // Non-EAP EAPOL packet (key exchange, etc.)
        snprintf(s_status, sizeof(s_status), "802.1X active");
    }
}

bool eapol_detected(void)
{
    return s_detected;
}

const char *eapol_get_status(void)
{
    return s_detected ? s_status : NULL;
}

void eapol_reset(void)
{
    s_detected = false;
    s_status[0] = '\0';
}
