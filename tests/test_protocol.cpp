/* Host tests include the real sketch. Hardware classes are replaced only
 * at the I/O boundary; these tests do not validate real modem timing. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <deque>
#include <string>
#include <vector>
#include <stdexcept>
#define P10_HOST_TEST
#define DEBUG_ENABLE 0
#define F(x) x
static unsigned long clockMs;
unsigned long millis() { return clockMs; }
void delay(unsigned long ms) { clockMs += ms; }
struct SerialStub {
    std::deque<char> rx;
    std::vector<std::string> tx;
    void begin(int) {}
    int available() { return (int)rx.size(); }
    int read() { char c=rx.front(); rx.pop_front(); return (unsigned char)c; }
    void println(const char *s) { tx.push_back(s); }
    void feed(const std::string &s) { for (char c:s) rx.push_back(c); }
} Serial2;
struct SoftDMD {
    SoftDMD(int,int) {}
    void clearScreen() {} void selectFont(const uint8_t*) {}
    void drawString(int,int,const char*) {} void setBrightness(int) {} void begin() {}
    int stringWidth(const char *s) { return (int)strlen(s)*6; }
};
const uint8_t SystemFont5x7[]={0};
struct FakeEEPROM {
    uint8_t bytes[1024]; int remaining=-1;
    FakeEEPROM() { memset(bytes,0xff,sizeof(bytes)); }
    uint8_t read(int a) { return bytes[a]; }
    void update(int a,uint8_t v) {
        if (remaining == 0) throw std::runtime_error("power cut");
        if (remaining > 0) --remaining;
        bytes[a]=v;
    }
} EEPROM;
#include "../SMS_P10_NoticeBoard/SMS_P10_NoticeBoard.ino"
static int checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n",__LINE__,#x); } } while (0)
void reset() {
    clockMs=1000; Serial2.rx.clear(); Serial2.tx.clear();
    smsState=SMS_WAIT_HEADER; smsStateTime=clockMs; readIndex=-1;
    bodyRemaining=bodyStored=gsmLineLen=0;
    captureBody=captureComplete=gsmLineOverflow=smsAvailable=false;
    smsBuffer[0]='\0'; ADMIN_NUMBERS[0]="+919876543210";
    displayState=SHOW_IDLE_TEXT; pollDue=false; lastPollTime=clockMs;
}
std::string header(int index,const char *sender,size_t n) {
    char h[200]; snprintf(h,sizeof h,"+CMGL: %d,\"REC UNREAD\",\"%s\",\"\",\"26/09/23,12:00:00+22\",145,%u\r\n",index,sender,(unsigned)n);
    return h;
}
void feed(const std::string &s) { Serial2.feed(s); gsmRxProcess(); }
int main() {
    const char *bodies[]={"OK","ERROR","+CMTI: \"SM\",9","+CMGL: fake","HELLO\r\nOK\r\nERROR","\r\n"};
    for (const char *body:bodies) {
        reset(); feed(header(4,ADMIN_NUMBERS[0],strlen(body)));
        // Feed individual bytes to exercise fragmented UART arrival.
        for (const char *c=body;*c;++c) feed(std::string(1,*c));
        CHECK(!smsAvailable); CHECK(bodyRemaining==0);
        feed("\r\nOK\r\n"); CHECK(smsAvailable);
        std::string expected(body); for(char &c:expected) if(c=='\r'||c=='\n') c=' ';
        CHECK(expected==smsBuffer); CHECK(Serial2.tx.back()=="AT+CMGD=4");
    }
    reset(); feed(header(1,ADMIN_NUMBERS[0],3)+"ONE\r\n"+header(2,ADMIN_NUMBERS[0],5)+"ERROR\r\nOK\r\n");
    CHECK(smsAvailable); CHECK(!strcmp(smsBuffer,"ONE")); CHECK(Serial2.tx.back()=="AT+CMGD=1");
    reset(); feed(header(1,"0",2)+"OK\r\n"+header(2,ADMIN_NUMBERS[0],3)+"TWO\r\nOK\r\n");
    CHECK(!smsAvailable); CHECK(Serial2.tx.back()=="AT+CMGD=1");
    reset(); feed(header(3,ADMIN_NUMBERS[0],400)+std::string(400,'A')+"\r\nOK\r\n");
    CHECK(smsAvailable); CHECK(strlen(smsBuffer)==299); CHECK(bodyRemaining==0);
    reset(); feed(header(3,ADMIN_NUMBERS[0],5)+"AB"); clockMs+=SMS_STATE_TIMEOUT_MS+1; loop();
    CHECK(!smsAvailable); CHECK(readIndex==-1); CHECK(bodyRemaining==0); CHECK(Serial2.tx.empty());
    reset(); feed("+CMGL: 1,\"REC UNREAD\",\"+919876543210\",\"\",\"date\"\r\nOK\r\n");
    CHECK(!smsAvailable); CHECK(readIndex==-1); CHECK(Serial2.tx.empty());
    char tiny[4]; CHECK(!extractQuoted("\"123456\"",1,tiny,sizeof tiny)); CHECK(tiny[0]=='\0');
    CHECK(!extractQuoted("\"a\"",1,tiny,0));

    FakeEEPROM base; char restored[300];
    CHECK(!MessageStore::load(base,restored,sizeof restored));
    MessageStore::save(base,"OLD",NULL);
    CHECK(MessageStore::load(base,restored,sizeof restored)); CHECK(!strcmp(restored,"OLD"));
    for(int cut=0;cut<=13;++cut) {
        FakeEEPROM ee=base; ee.remaining=cut;
        try { MessageStore::save(ee,"NEW",NULL); } catch(const std::runtime_error &) {}
        CHECK(MessageStore::load(ee,restored,sizeof restored));
        CHECK(!strcmp(restored,cut<13?"OLD":"NEW"));
    }
    MessageStore::save(base,"NEW",NULL);
    CHECK(MessageStore::load(base,restored,sizeof restored)); CHECK(!strcmp(restored,"NEW"));
    base.bytes[MessageStore::stride+MessageStore::header]^=1;
    CHECK(MessageStore::load(base,restored,sizeof restored)); CHECK(!strcmp(restored,"OLD"));
    // Corrupt the only remaining valid record: fail closed.
    base.bytes[MessageStore::header]^=1;
    CHECK(!MessageStore::load(base,restored,sizeof restored));
    printf("%d checks, %d failed\n",checks,failures);
    return failures?1:0;
}
