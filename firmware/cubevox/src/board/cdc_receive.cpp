#include "board/cdc_receive.h"

#include "usbd_cdc.h"
#include "usbd_cdc_if.h"

#ifdef DTR_TOGGLING_SEQ
#error "cdc_receive.cpp replaces the core's receive callback, which runs the DTR toggling hook on this board"
#endif

// The core's receive state in usbd_cdc_if.c. True while the OUT endpoint is armed for the next packet.
extern "C" __IO bool receivePended;

namespace {

// Runs in the USB interrupt for each packet the OUT endpoint received into the block the queue
// reserved. With room for another packet it arms the endpoint at once. With no room it leaves the
// endpoint off, so the host's next packets wait (NAK) until USBSerial::read frees a block and
// CDC_resume_receive arms it from the main loop. Only one side ever arms a disabled endpoint.
//
// The core's own callback arms the endpoint without a buffer when the queue is full
// (USBD_CDC_ClearBuffer). That packet lands at address 0, its length moves the queue's write
// position over unread bytes, and the next USBSerial::read arms the still-enabled endpoint a second
// time. Under a flood the second arming loses the transfer-complete interrupt, receivePended stays
// true, and nothing arms the endpoint again.
int8_t receive(uint8_t* buf, uint32_t* len) {
  (void)buf;
  CDC_ReceiveQueue_CommitBlock(&ReceiveQueue, static_cast<uint16_t>(*len));
  receivePended = false;
  CDC_resume_receive();
  return USBD_OK;
}

}  // namespace

void cdcReceiveInstall() { USBD_CDC_fops.Receive = receive; }
