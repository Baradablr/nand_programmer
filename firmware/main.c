/*  Copyright (C) 2017 Bogdan Bogush <bogdan.s.bogush@gmail.com>
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 */

/* NAND */
#include "fsmc_nand.h"
#include "chip_db.h"
/* SPL */
#include <stm32f10x.h>
/* USB */
#include <usb_lib.h>
#include <usb_pwr.h>
#include "hw_config.h"
/* STD */
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#define NAND_PAGE_NUM      2
#define NAND_BUFFER_SIZE   (NAND_PAGE_NUM * NAND_PAGE_SIZE)

#define USB_BUF_SIZE 60

enum
{
    CMD_NAND_READ_ID = 0x00,
    CMD_NAND_ERASE   = 0x01,
    CMD_NAND_READ    = 0x02,
    CMD_NAND_WRITE_S = 0x03,
    CMD_NAND_WRITE_D = 0x04,
    CMD_NAND_WRITE_E = 0x05,
    CMD_NAND_SELECT  = 0x06,
};

typedef struct __attribute__((__packed__))
{
    uint8_t code;
} cmd_t;

typedef struct __attribute__((__packed__))
{
    cmd_t cmd;
    uint32_t addr;
} write_start_cmd_t;

typedef struct __attribute__((__packed__))
{
    cmd_t cmd;
    uint8_t len;
    uint8_t data[];
} write_data_cmd_t;

typedef struct __attribute__((__packed__))
{
    cmd_t cmd;
} write_end_cmd_t;

typedef struct __attribute__((__packed__))
{
    cmd_t cmd;
    uint32_t addr;
    uint32_t len;
} read_cmd_t;

typedef struct __attribute__((__packed__))
{
    cmd_t cmd;
    uint32_t chip_num;
} select_cmd_t;

enum
{
    RESP_DATA   = 0x00,
    RESP_STATUS = 0x01,
};

typedef struct __attribute__((__packed__))
{
    uint8_t code;
    uint8_t info;
    uint8_t data[];
} resp_t;

enum
{
    STATUS_OK    = 0x00,
    STATUS_ERROR = 0x01,
};

typedef struct __attribute__((__packed__))
{
    resp_t header;
    nand_id_t nand_id;
} resp_id_t;

typedef struct
{
    nand_addr_t addr;
    int is_valid;
} prog_addr_t;

typedef struct
{
    uint8_t buf[NAND_PAGE_SIZE];
    uint32_t offset;
} page_t;

typedef struct
{
    uint8_t *rx_buf;
    uint8_t rx_buf_size;
    uint8_t *tx_buf;
    uint8_t tx_buf_size;
} usb_t;

nand_addr_t nand_write_read_addr = { 0x00, 0x00, 0x00 };
uint8_t nand_write_buf[NAND_BUFFER_SIZE], nand_read_buf[NAND_BUFFER_SIZE];

extern __IO uint8_t Receive_Buffer[USB_BUF_SIZE];
uint8_t usb_send_buf[USB_BUF_SIZE];
static uint32_t selected_chip = CHIP_ID_NONE;

static void jtag_init()
{
    /* Enable JTAG in low power mode */
    DBGMCU_Config(DBGMCU_SLEEP | DBGMCU_STANDBY | DBGMCU_STOP, ENABLE);
}

static void usb_init(usb_t *usb)
{
    Set_System();
    Set_USBClock();
    USB_Interrupts_Config();
    USB_Init();

    usb->rx_buf = (uint8_t *)Receive_Buffer;
    usb->rx_buf_size = sizeof(Receive_Buffer);
    usb->tx_buf = (uint8_t *)usb_send_buf;
    usb->tx_buf_size = sizeof(usb_send_buf);
}

static int make_status(usb_t *usb, int is_ok)
{
    resp_t status = { RESP_STATUS,  is_ok ? STATUS_OK : STATUS_ERROR };
    size_t len = sizeof(status);

    if (len > usb->tx_buf_size)
        return -1;

    memcpy(usb->tx_buf, &status, len);

    return len;
}

static int cmd_nand_read_id(usb_t *usb)
{
    resp_id_t resp;
    size_t resp_len = sizeof(resp);

    if (selected_chip == CHIP_ID_NONE)
        goto Error;

    if (usb->tx_buf_size < resp_len)
        goto Error;

    resp.header.code = RESP_DATA;
    resp.header.info = resp_len - sizeof(resp.header);
    nand_read_id(&resp.nand_id);

    memcpy(usb->tx_buf, &resp, resp_len);

    return resp_len;

Error:
    return make_status(usb, 0);
}

static int cmd_nand_erase(usb_t *usb)
{
    uint32_t ret = -1;

    if (selected_chip == CHIP_ID_NONE)
        goto Exit;

    /* Erase the NAND first Block */
    if (nand_erase_block(nand_write_read_addr) != NAND_READY)
        goto Exit;

    ret = 0;
Exit:
    return make_status(usb, !ret);
}

static int cmd_nand_write_start(usb_t *usb, prog_addr_t *prog_addr,
    page_t *page)
{
    write_start_cmd_t *write_start_cmd = (write_start_cmd_t *)usb->rx_buf;

    if (nand_raw_addr_to_nand_addr(write_start_cmd->addr, &prog_addr->addr)
        != NAND_VALID_ADDRESS)
    {
        return -1;
    }
    prog_addr->is_valid = 1;

    page->offset = write_start_cmd->addr % sizeof(page->buf);
    memset(page->buf, 0, sizeof(page->buf));

    return 0;
}

static int cmd_nand_write_data(usb_t *usb, prog_addr_t *prog_addr, page_t *page)
{
    uint32_t status, write_len, bytes_left;
    uint32_t page_size = sizeof(page->buf);    
    write_data_cmd_t *write_data_cmd = (write_data_cmd_t *)usb->rx_buf;

    if (write_data_cmd->len + offsetof(write_data_cmd_t, data) >
        usb->rx_buf_size)
    {
        return -1;
    }

    if (!prog_addr->is_valid)
        return -1;

    if (page->offset + write_data_cmd->len > page_size)
        write_len = page_size - page->offset;
    else
        write_len = write_data_cmd->len;

    memcpy(page->buf + page->offset, write_data_cmd->data, write_len);
    page->offset += write_len;

    if (page->offset == page_size)
    {
        status = nand_write_small_page(page->buf, prog_addr->addr, 1);
        if (!(status & NAND_READY))
            return -1;

        status = nand_addr_inc(&prog_addr->addr);
        if (!(status & NAND_VALID_ADDRESS))
            prog_addr->is_valid = 0;

        page->offset = 0;
        memset(page->buf, 0, page_size);
    }

    bytes_left = write_data_cmd->len - write_len;
    if (bytes_left)
    {
        memcpy(page->buf, write_data_cmd->data + write_len, bytes_left);
        page->offset += bytes_left;
    }

    return 0;
}

static int cmd_nand_write_end(prog_addr_t *prog_addr, page_t *page)
{
    uint32_t status;

    if (!prog_addr->is_valid)
        return 0;

    prog_addr->is_valid = 0;

    if (!page->offset)
        return 0;

    status = nand_write_small_page(page->buf, prog_addr->addr, 1);
    if (!(status & NAND_READY))
       return -1;

    return 0;
}

static int cmd_nand_write(usb_t *usb)
{
    static prog_addr_t prog_addr;
    static page_t page;
    cmd_t *cmd = (cmd_t *)usb->rx_buf;
    int ret = -1;

    if (selected_chip == CHIP_ID_NONE)
        goto Exit;

    switch (cmd->code)
    {
    case CMD_NAND_WRITE_S:
        ret = cmd_nand_write_start(usb, &prog_addr, &page);
        break;
    case CMD_NAND_WRITE_D:
        ret = cmd_nand_write_data(usb, &prog_addr, &page);
        if (!ret)
            return 0;
        break;
    case CMD_NAND_WRITE_E:
        ret = cmd_nand_write_end(&prog_addr, &page);
        break;
    default:
        break;
    }

Exit:
    return make_status(usb, !ret);
}

static int cmd_nand_read(usb_t *usb)
{
    prog_addr_t prog_addr;
    static page_t page;
    uint32_t status, write_len;
    uint32_t page_size = sizeof(page.buf);
    uint32_t resp_header_size = offsetof(resp_t, data);
    uint32_t tx_data_len = usb->tx_buf_size - resp_header_size;
    read_cmd_t *read_cmd = (read_cmd_t *)usb->rx_buf;
    resp_t *resp = (resp_t *)usb->tx_buf;

    if (selected_chip == CHIP_ID_NONE)
        goto Error;

    if (nand_raw_addr_to_nand_addr(read_cmd->addr, &prog_addr.addr)
        != NAND_VALID_ADDRESS)
    {
        goto Error;
    }

    page.offset = read_cmd->addr % page_size;

    resp->code = RESP_DATA;

    while (read_cmd->len)
    {
        status = nand_read_small_page(page.buf, prog_addr.addr, 1);
        if (!(status & NAND_READY))
            goto Error;

        while (page.offset < page_size && read_cmd->len)
        {
            if (page_size - page.offset >= tx_data_len)
                write_len = tx_data_len;
            else
                write_len = page_size - page.offset;

            if (write_len > read_cmd->len)
                write_len = read_cmd->len;
 
            memcpy(resp->data, page.buf + page.offset, write_len);

            while (!CDC_IsPacketSent());

            resp->info = write_len;
            CDC_Send_DATA(usb->tx_buf, resp_header_size + write_len);

            page.offset += write_len;
            if (page.offset == page_size)
                page.offset = 0;
            read_cmd->len -= write_len;
        }

        if (read_cmd->len)
        {
            status = nand_addr_inc(&prog_addr.addr);
            if (!(status & NAND_VALID_ADDRESS))
                goto Error;
        }
    }

    return 0;

Error:
    return make_status(usb, 0);
}

static int cmd_nand_select(usb_t *usb)
{
    select_cmd_t *select_cmd = (select_cmd_t *)usb->rx_buf;
    int ret = 0;

    if (select_cmd->chip_num < CHIP_ID_LAST)
    {
        selected_chip = select_cmd->chip_num;
        nand_init(selected_chip);
    }
    else
        ret = -1;

    return make_status(usb, !ret);
}

static int usb_cmd_handler(usb_t *usb)
{
    cmd_t *cmd = (cmd_t *)usb->rx_buf;
    int ret = -1;

    switch (cmd->code)
    {
    case CMD_NAND_READ_ID:
        ret = cmd_nand_read_id(usb);
        break;
    case CMD_NAND_ERASE:
        ret = cmd_nand_erase(usb);
        break;
    case CMD_NAND_READ:
        ret = cmd_nand_read(usb);
        break;
    case CMD_NAND_WRITE_S:
    case CMD_NAND_WRITE_D:
    case CMD_NAND_WRITE_E:
        ret = cmd_nand_write(usb);
        break;
    case CMD_NAND_SELECT:
        ret = cmd_nand_select(usb);
        break;
    default:
        break;
    }

    return ret;
}

static void usb_handler(usb_t *usb)
{
    int len;

    if (!USB_IsDeviceConfigured())
        return;

    CDC_Receive_DATA();
    if (!CDC_ReceiveDataLen())
        return;

    len = usb_cmd_handler(usb);
    if (len <= 0)
        goto Exit;

    if (CDC_IsPacketSent())
        CDC_Send_DATA(usb->tx_buf, len);

Exit:
    CDC_ReceiveDataAck();
}

int main()
{
    static usb_t usb;

    jtag_init();

    usb_init(&usb);

    while (1)
        usb_handler(&usb);

    return 0;
}
