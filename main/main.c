#include "ets_sys.h"
#include "osapi.h"
#include "os_type.h"
#include "user_interface.h"
#include "driver/uart.h"
#include "espconn.h"

#include "config.h"
#include "dns.h"

#ifdef ENABLE_GDB
#include "../esp-gdbstub/gdbstub.h"
#endif

#define MESSAGE_QUEUE_LEN 1
os_event_t message_queue[MESSAGE_QUEUE_LEN];

static esp_udp dns_udp;
static struct espconn dns_conn;
static struct resource_record catchall[1];

/**
 * Main code function
 */
static void ICACHE_FLASH_ATTR
loop(os_event_t *events)
{
        os_delay_us(10000);

        system_os_task(loop, 0, message_queue, MESSAGE_QUEUE_LEN);
        system_os_post(0, 0, 0);
}

/**
 * Main code init function
 */
static void ICACHE_FLASH_ATTR
init(os_event_t *events)
{

        /* Start main code loop */
        system_os_task(loop, 0, message_queue, MESSAGE_QUEUE_LEN);
        system_os_post(0, 0, 0);
}


/* Dio_to_qio unused, overwrite to reduce iRAM usage */
void
user_spi_flash_dio_to_qio_pre_init(void)
{

}


void ICACHE_FLASH_ATTR
user_dns_rcv(void *arg, char *pdata, uint16 len)
{
        bool err;
        uint8 *answer;
        uint16 anlen;
        remot_info *remote;
        struct espconn *peer = arg;


        dns_parse(pdata, len);
        dns_find_answers();
        dns_dump();
        err = dns_write_response(&answer, &anlen);
        if(err) {
                os_printf("ERROR: %s\n", dns_errstr());
                return;
        }

        if( espconn_get_connection_info(peer, &remote, 0) == ESPCONN_OK ) {
             peer->proto.udp->remote_port  = remote->remote_port;
             peer->proto.udp->remote_ip[0] = remote->remote_ip[0];
             peer->proto.udp->remote_ip[1] = remote->remote_ip[1];
             peer->proto.udp->remote_ip[2] = remote->remote_ip[2];
             peer->proto.udp->remote_ip[3] = remote->remote_ip[3];
             espconn_sent(peer, answer, anlen);
        }
}

void ICACHE_FLASH_ATTR
user_dns_init()
{
        sint8 ret;

        dns_udp.local_ip[0] = 1;
        dns_udp.local_ip[1] = 10;
        dns_udp.local_ip[2] = 10;
        dns_udp.local_ip[3] = 10;
        dns_udp.local_port = 53;
        dns_conn.proto.udp = &dns_udp;
        dns_conn.type = ESPCONN_UDP;
        dns_conn.recv_callback = user_dns_rcv;

        catchall[0].catchall = true;
        catchall[0].ttl      = 1;
        catchall[0].type     = 1;
        catchall[0].class    = 1;
        catchall[0].rdata[0] = 10;
        catchall[0].rdata[1] = 10;
        catchall[0].rdata[2] = 10;
        catchall[0].rdata[3] = 1;
        catchall[0].rdlength = 4;
        dns_records = catchall;
        dns_record_count = 1;

        ret = espconn_create(&dns_conn);
        switch(ret) {
        case ESPCONN_MEM:
                os_printf("dns_udp err: no mem\n");
                break;
        case ESPCONN_ISCONN:
                os_printf("dns_udp err: is conn\n");
                break;
        case ESPCONN_ARG:
                os_printf("dns_udp err: arg\n");
                break;
        }
}

/**
 * Init function
 */
void ICACHE_FLASH_ATTR
user_init()
{
        char                 ssid[32]   = AP_SSID;
        uint8                macaddr[6] = { 0xC0, 0xFE, 0xC0, 0xFE, 0xC0, 0xFE };
        struct softap_config ap_conf;
        struct ip_info       ip_conf;

        uart_init(BIT_RATE_115200, BIT_RATE_115200);

#ifdef ENABLE_GDB
        gdbstub_init();
#endif

        /* Configure access point */
        ap_conf.channel         = AP_CHANNEL;
        ap_conf.beacon_interval = AP_BEACON_MS;
        ap_conf.authmode        = AUTH_OPEN;
        ap_conf.ssid_len        = sizeof(AP_SSID)-1;
        ap_conf.max_connection  = 4;
        os_memcpy(ap_conf.ssid, ssid, sizeof(AP_SSID));
        wifi_set_macaddr(SOFTAP_IF, macaddr);
        wifi_set_opmode(SOFTAP_MODE);
        wifi_softap_set_config_current(&ap_conf);

        /* Configure DHCP address range */
        wifi_softap_dhcps_stop();
        IP4_ADDR(&ip_conf.ip,      10, 10, 10, 1);
        IP4_ADDR(&ip_conf.gw,      10, 10, 10, 1);
        IP4_ADDR(&ip_conf.netmask, 255, 255, 255, 0);
        wifi_set_ip_info(SOFTAP_IF, &ip_conf);
        wifi_softap_dhcps_start();

        user_dns_init();

        /* Start os task */
        system_os_task(init, 0, message_queue, MESSAGE_QUEUE_LEN);
        system_os_post(0, 0, 0);
}
