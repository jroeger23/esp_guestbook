/* LWIP definitions */
#define LWIP_RAW 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define NO_SYS   0

#include "dns.h"
#include "dns_def.h"

#include "os_type.h"
#include "user_interface.h"
#include "lwip/def.h"

/* OpCodes */
#define IS_STD_QUERY           ((header[0] & 0b01111000) == 0b1000)
#define IS_STATUS              ((header[0] & 0b01111000) == 0b10000)
#define IS_NOTIFY              ((header[0] & 0b01111000) == 0b100000)
#define IS_UPDATE              ((header[0] & 0b01111000) == 0b101000)
#define SET_OPCODE(header,val) (header[0] = (header[0] & 0b10000111) | ((val<<3) & 0b01111000))
/* Flags */
#define IS_RESPONSE          (header[0] >> 7)
#define IS_TRUNC             ((header[0] & 0b00000010))
#define SET_QR(header, val)  (header[0] = (header[0] & 0b01111111) | ((val<<7) & 0b10000000))
#define SET_AA(header, val)  (header[0] = (header[0] & 0b11111011) | ((val<<2) & 0b00000100))
/* Rcode */
#define SET_RCODE(header,val) header[1] &= 0b11110000; header[1] |= (val & 0b00001111)



/* Packet content */
static uint8  id[2];
static uint8  header[2];
static uint16 qdcount;
static uint16 ancount;
static uint16 nscount;
static uint16 arcount;
static struct question questions[MAX_QUESTIONS];


static struct resource_record *answer_records[MAX_QUESTIONS];
static uint8 dns_resp_buf[MAX_RESPONSE_SIZE];
static uint16 written;

enum dns_error dns_error;
struct resource_record *dns_records;
uint16 dns_record_count;


void ICACHE_FLASH_ATTR
dns_dump()
{
        os_printf("STATUS: %s\n", dns_errstr());
        os_printf("ID:      %02X:%02X\n", id[0], id[1]);
        os_printf("QDCOUNT: %d\n", qdcount);
        os_printf("ANCOUNT: %d\n", ancount);
        os_printf("NSCOUNT: %d\n", nscount);
        os_printf("ARCOUNT: %d\n", arcount);
        os_printf("----Questions----\n");

        for(int i = 0; i < qdcount; ++i) {
                for(int l = 0; l < questions[i].n_label; ++l) {
                        os_printf("  %s\n", questions[i].labels[l]);
                }
                os_printf("    TYPE:  %d\n", questions[i].type);
                os_printf("    CLASS: %d\n", questions[i].class);
        }

        os_printf("-----------------\n");
        os_printf("----Answers----\n");

        for(int i = 0; i < ancount; ++i) {
                os_printf("RData: ");
                for(int x = 0; x < answer_records[i]->rdlength; x++) {
                        os_printf("%02X", answer_records[i]->rdata[x]);
                }
        }

        os_printf("\n-----------------\n");
}

/**
 * Return a string describing dns_error
 */
char* ICACHE_FLASH_ATTR
dns_errstr()
{
        switch(dns_error) {
        case DNSE_OK:
                return "OK";
        case DNSE_ERROR:
                return "ERROR";
        case DNSE_PACKET_TOO_SMALL:
                return "packet too small to proceed parsing";
        case DNSE_LABEL_LEN_OVERFLOW:
                return "length of label is too long";
        case DNSE_NAME_LEN_OVERFLOW:
                return "length of name is too long";
        case DNSE_UNIMPLEMENTED:
                return "requested feature, but it's not implemented";
        case DNSE_RESP_BUF_FULL:
                return "response buffer is full";
        }
}


bool ICACHE_FLASH_ATTR
dns_check_header()
{
        bool ret = false;

        if( IS_TRUNC || IS_NOTIFY || IS_STATUS || IS_UPDATE ) {
                dns_error = DNSE_UNIMPLEMENTED;
                ret = true;
        }
        if( IS_RESPONSE ) {
                dns_error = DNSE_ERROR;
                ret = true;
        }

        return ret;
}

/**
 * Parse all labels starting at data and write to question
 * @param data data to start reading from
 * @param len bytes available
 * @param q the question to be filled with labels
 * @return new data pointer on success or NULL on error
 */
static char * ICACHE_FLASH_ATTR
dns_parse_labels(char *data, uint16 len, struct question *q)
{
        uint8  l_idx = 0;
        uint8  n_idx = 0;
        uint8  cur_len;

        q->n_label = 0;


        for(;;) {
                if(len < 1)
                { dns_error = DNSE_PACKET_TOO_SMALL; return NULL; }

                /* Read and check label len */
                cur_len = *data++;
                q->name[n_idx++] = cur_len;
                if(cur_len > len)
                { dns_error = DNSE_LABEL_LEN_OVERFLOW; return NULL; }
                if(cur_len+n_idx > MAX_NAME_LEN)
                { dns_error = DNSE_NAME_LEN_OVERFLOW;  return NULL; }
                if(cur_len == 0) return data;
                len -= cur_len+1;

                /* Read label */
                q->labels[l_idx++] = q->name+n_idx;
                q->n_label++;
                while(cur_len--) {
                        q->name[n_idx++] = *data++;
                }
        }
}

/**
 * Parse all question records
 * @param data data to start reading from
 * @param len bytes available
 */
static bool ICACHE_FLASH_ATTR
dns_parse_questions(char *data, uint16 len)
{
        uint16 count;
        uint8  q_idx = 0;
        char   *ret;
        uint16 aligned;

        count = qdcount > MAX_QUESTIONS ? MAX_QUESTIONS : qdcount;

        /* Parse each question record */
        while(q_idx < count) {

                ret = dns_parse_labels(data, len, &questions[q_idx]);
                if(!ret) return true;
                len -= ret-data;
                data=ret;

                if(len < 4)
                { dns_error = DNSE_PACKET_TOO_SMALL ; return true; }
                aligned = *data++;
                aligned |= (*data++) << 8;
                questions[q_idx].type  = ntohs( aligned );
                aligned = *data++;
                aligned |= (*data++) << 8;
                questions[q_idx].class = ntohs( aligned );
                len  -= 4;

                q_idx++;
        }
}

void ICACHE_FLASH_ATTR
dns_find_resource(char *name, struct resource_record **r)
{
        struct resource_record *catchall = NULL;

        *r = NULL;

        /* Search in each records name for the given name  */
        for(int i = 0; i < dns_record_count; ++i) {
                if( dns_records[i].catchall )
                        catchall = &dns_records[i];
                if( !os_strncmp(name, dns_records[i].name, MAX_NAME_LEN) ) {
                        /* Found matching record */
                        *r = &dns_records[i];
                        return;
                }
        }

        if( catchall ) {
                /* Copy desired resource name */
                catchall->namelen = os_strlen(name)+1; // Include \0
                os_strncpy(catchall->name, name, catchall->namelen < MAX_NAME_LEN
                           ? catchall->namelen : MAX_NAME_LEN);

                *r = catchall;
        }
}

/**
 * Parse data as DNS packet
 * @param data the packet data
 * @param len size of data
 * @return false = success ; true = error (@see dns_errstr())
 */
bool ICACHE_FLASH_ATTR
dns_parse(char *data, uint16 len)
{
        bool err;

        char *dptr = data;

        dns_error = DNSE_OK;

        if(len < 12)
        { dns_error = DNSE_PACKET_TOO_SMALL; return true; }

        /* Copy id */
        id[0] = *dptr++;
        id[1] = *dptr++;

        /* Copy header */
        header[0] = *dptr++;
        header[1] = *dptr++;

        /* Read counts (data is already aligned) */
        qdcount = ntohs( *((uint16*)dptr)   );
        ancount = ntohs( *((uint16*)dptr+2) );
        nscount = ntohs( *((uint16*)dptr+4) );
        arcount = ntohs( *((uint16*)dptr+6) );
        dptr += 8;

        err = dns_check_header();
        if(err) return err;

        err = dns_parse_questions(dptr, len-(dptr-data));

        return err;
}

/**
 * Generates answers in answers[]
 * for each question in questions[]
 */
static bool ICACHE_FLASH_ATTR
dns_write_answers()
{
        uint16 aligned;
        uint32 aligned32;
        uint8 *head = dns_resp_buf+written;

        for(int i = 0; i < qdcount; ++i) {
                if(written+answer_records[i]->namelen > MAX_RESPONSE_SIZE)
                { dns_error = DNSE_RESP_BUF_FULL; return true; }

                /* First write name field */
                os_strncpy((char*)head, answer_records[i]->name, answer_records[i]->namelen);
                head    += answer_records[i]->namelen;
                written += answer_records[i]->namelen;

                /* Reserve static size (type, class, ttl, rdlength) */
                if(written+12 > MAX_RESPONSE_SIZE)
                { dns_error = DNSE_RESP_BUF_FULL; return true; }

                /* Copy type and class (aligned) */
                aligned = htons( answer_records[i]->type );
                head[0] = aligned & 0xFF; head[1] = aligned >> 8;
                aligned = htons( answer_records[i]->class );
                head[2] = aligned & 0xFF; head[3] = aligned >> 8;

                /* Copy ttl (aligned) */
                aligned32 = htonl( answer_records[i]->ttl );
                head[4] = aligned32 & 0xFF;         head[5] = (aligned32 >> 8) & 0xFF;
                head[6] = (aligned32 >> 16) & 0xFF; head[7] = aligned32 >> 24;

                /* rdlength (aligned) */
                aligned = htons( answer_records[i]->rdlength );
                head[8] = aligned & 0xFF; head[9]  = aligned >> 8;

                written += 10;
                head    += 10;

                if(written+answer_records[i]->rdlength > MAX_RESPONSE_SIZE)
                { dns_error = DNSE_RESP_BUF_FULL; return true; }

                /* rdata */
                os_memcpy(head, answer_records[i]->rdata, answer_records[i]->rdlength);

                written += answer_records[i]->rdlength;
                head    += answer_records[i]->rdlength;

        }

        return false;
}


void ICACHE_FLASH_ATTR
dns_find_answers()
{
        bool ret;
        uint16 count = qdcount > MAX_QUESTIONS ? MAX_QUESTIONS : qdcount;

        ancount = 0;

        for(int i = 0; i < qdcount; ++i) {
                dns_find_resource(questions[i].name, &answer_records[ancount]);
                /* increase answer count if found */
                if(answer_records[ancount]) {
                        ancount += 1;
                }
        }
}

/**
 * Generate response and point buf to it
 * @param buf will point to the packet buffer
 * @param len will be the lengh of the packet buffer
 * @return true = error ; false = success
 */
bool ICACHE_FLASH_ATTR
dns_write_response(uint8 **buf, uint16 *len)
{
        bool   err;
        uint8  *head   = dns_resp_buf;

        if( 12 > MAX_RESPONSE_SIZE )
        { dns_error = DNSE_RESP_BUF_FULL; return true; }

        /* Copy id */
        head[0] = id[0];
        head[1] = id[1];
        head += 2;

        /* Write header */
        head[0] = head[1] = 0;
        SET_QR(head, 1);
        SET_AA(head, 1);

        /* Set OpCode to update */
        SET_OPCODE(head, 0);

        /* Response code */
        switch(dns_error) {
        case DNSE_OK:
                SET_RCODE(head, RC_NoError); break;
        case DNSE_ERROR:
                SET_RCODE(head, RC_ServFail); break;
        case DNSE_PACKET_TOO_SMALL:
                SET_RCODE(head, RC_BADTRUC); break;
        case DNSE_LABEL_LEN_OVERFLOW:
                SET_RCODE(head, RC_ServFail); break;
        case DNSE_NAME_LEN_OVERFLOW:
                SET_RCODE(head, RC_ServFail); break;
        case DNSE_UNIMPLEMENTED:
                SET_RCODE(head, RC_NotImp); break;
        default:
                SET_RCODE(head, RC_ServFail); break;
        }
        head += 2;

        /* Counter */
        head[0] = 0;               // qdcount
        head[1] = 0;               // qdcount
        head[3] =  ancount & 0xFF; // ancount ( answer for each question )
        head[2] |= ancount << 8;   // ancount
        head[4] = 0;               // nscount
        head[5] = 0;               // nscount
        head[6] = 0;               // arcount
        head[7] = 0;               // arcount

        written = 12;

        /* Only write header and counter on error */
        if(dns_error != DNSE_OK) {
                /* Set answer count to 0 */
                head[2] = 0;
                head[3] = 0;

                *buf = dns_resp_buf;
                *len = written;
                return false;
        }

        head += 8;

        err = dns_write_answers();
        if(err) return err;

        *buf = dns_resp_buf;
        *len = written;

        //TODO: Return code for one or multiple names not found

        return false;
}
