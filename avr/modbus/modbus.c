﻿/*
* modbus.c
*
* Created: 27.10.2018 17:13:34
*  Author: Evgeny
*/
#include "Globals.h"
#include "Modbus.h"
#include "HAL/Timers.h"
#include "HAL/Uart.h"

#include <string.h>

static const int ADDR_OFFSET     = 0;
static const int FUNC_OFFSET     = 1;
static const int DATA_OFFSET     = 2;
static const int COUNT_OFFSET    = 4;
static const int FC_COILS_OFFSET = 7;

static const int OUTPUT_LEN_OFFSET      = 2;
static const int OUTPUT_DATA_OFFSET     = 3;
static const int OUTPUT_FCDATA1_OFFSET  = 2;
static const int OUTPUT_FCDATA2_OFFSET  = 4;

static const int ERROR_FLAG         = 0x80;
static const int NO_ERRORS          = 0x00;
static const int ILLEGAL_FUNCTION   = 0x01;
static const int ILLEGAL_ADDRESS    = 0x02;
static const int ILLEGAL_DATA_VALUE = 0x03;

static const int HEADER_LEN = 3;
static const int FC05_LEN   = 6;
static const int COIL_ON    = 0xFF00;


#define READ_COIL_STATUS        0x01
#define READ_INPUT_STATUS       0x02
#define READ_INPUT_REGISTERS    0x04
#define FORCE_SINGLE_COIL       0x05
#define FORCE_MULTIPLE_COILS    0x0F

#define ntohs(a)            htons(a)
#define OUTPUT_BUFFER_SIZE (MODBUS_INPUTREG_COUNT*2+8)
#define UART_BUFFER_SIZE    32

uint16_t CRC16(uint8_t *p, uint16_t len);

static struct {
    uint8_t InputBuffer[UART_BUFFER_SIZE];
    int nBytesCount;
} stUartData;

static struct {
    int16_t nInputRegisters[MODBUS_INPUTREG_COUNT];
    uint8_t unCoils [MODBUS_COILS_COUNT];
    uint8_t unInputs [MODBUS_INPUTS_COUNT];
} stModbusData;

static uint16_t htons(uint16_t val)
{
    return (val << 8) | (val >> 8);
}

static int ReadCoilStatus(uint16_t unRegister, uint16_t unCount, uint8_t* pOutputBuffer, uint8_t* pOutputLen)
{
    int nErrorCode = ILLEGAL_ADDRESS;

    if ((unCount > 0) && ((unRegister + unCount) <= MODBUS_COILS_COUNT))
    {
        //эта версия поддерживает чтение не более 8 значений выходов, поэтому во фрейме фикс. длина 1 байт
        pOutputBuffer[OUTPUT_LEN_OFFSET] = 1;
        uint8_t CoilsValue = 0;

        for (int i = 0; i < unCount; ++i)
        {
            if (stModbusData.unCoils[unRegister + i])
            {
                CoilsValue |= (1 << i);
            }
        }

        pOutputBuffer[OUTPUT_DATA_OFFSET] = CoilsValue;
        *pOutputLen = HEADER_LEN + pOutputBuffer[OUTPUT_LEN_OFFSET];
        nErrorCode = NO_ERRORS;
    }

    return nErrorCode;
}

static int ReadInputStatus(uint16_t unRegister, uint16_t unCount, uint8_t* pOutputBuffer, uint8_t* pOutputLen)
{
    int nErrorCode = ILLEGAL_ADDRESS;

    if ((unCount > 0) && ((unRegister + unCount) <= MODBUS_INPUTS_COUNT))
    {
        //эта версия поддерживает чтение не более 8 значений входов, поэтому во фрейме фикс. длина 1 байт
        pOutputBuffer[OUTPUT_LEN_OFFSET] = 1;
        uint8_t InputsValue = 0;

        for (int i = 0; i < unCount; ++i)
        {
            if (stModbusData.unInputs[unRegister + i])
            {
                InputsValue |= (1 << i);
            }
        }

        pOutputBuffer[OUTPUT_DATA_OFFSET] = InputsValue;
        *pOutputLen = HEADER_LEN + pOutputBuffer[OUTPUT_LEN_OFFSET];
        nErrorCode = NO_ERRORS;
    }

    return nErrorCode;
}

static int ReadInputRegisters(uint16_t unRegister, uint16_t unCount, uint8_t* pOutputBuffer, uint8_t* pOutputLen)
{
    int nErrorCode = ILLEGAL_ADDRESS;

    if ((unCount > 0) && ((unRegister + unCount) <= MODBUS_INPUTREG_COUNT))
    {
        pOutputBuffer[OUTPUT_LEN_OFFSET] = unCount * sizeof(uint16_t);
        uint16_t* pOutputRegister = (uint16_t*)(pOutputBuffer + OUTPUT_DATA_OFFSET);

        for (int i = 0; i < unCount; ++i)
        {
            *pOutputRegister++ = htons(stModbusData.nInputRegisters[unRegister + i]);
        }

        *pOutputLen = HEADER_LEN + pOutputBuffer[OUTPUT_LEN_OFFSET];
        nErrorCode = NO_ERRORS;
    }

    return nErrorCode;
}

static int ForceSingeCoil(uint16_t unRegister, uint16_t unValue, uint8_t* pOutputBuffer, uint8_t* pOutputLen)
{
    int nErrorCode = ILLEGAL_ADDRESS;

    if (unRegister < MODBUS_COILS_COUNT)
    {
        if (unValue == COIL_ON)
        {
            stModbusData.unCoils[unRegister] = 1;
            nErrorCode = NO_ERRORS;
        }
        else if (unValue == 0)
        {
            stModbusData.unCoils[unRegister] = 0;
            nErrorCode = NO_ERRORS;
        }
        else {
            nErrorCode = ILLEGAL_DATA_VALUE;
        }

        if (NO_ERRORS == nErrorCode)
        {
            uint16_t* pOutputRegister = (uint16_t*)(pOutputBuffer + OUTPUT_FCDATA1_OFFSET);
            *pOutputRegister = htons(unRegister);
            uint16_t* pOutputValue = (uint16_t*)(pOutputBuffer + OUTPUT_FCDATA2_OFFSET);
            *pOutputValue = htons(unValue);
            *pOutputLen = FC05_LEN;
            nErrorCode = NO_ERRORS;
        }
    }

    return nErrorCode;
}

static int ForceMultipleCoils(uint16_t unRegister, uint16_t unCount, uint8_t unCoilsData, uint8_t* pOutputBuffer, uint8_t* pOutputLen)
{
    int nErrorCode = ILLEGAL_ADDRESS;

    if ((unCount > 0) && ((unRegister + unCount) <= MODBUS_COILS_COUNT))
    {
        //будет работать только до 8 выходов
        for (int i = 0; i < unCount; ++i)
        {
            if (unCoilsData & (1 << i))
            {
                stModbusData.unCoils[unRegister + i] = 1;
            }
            else
            {
                stModbusData.unCoils[unRegister + i] = 0;
            }
        }

        uint16_t* pOutputRegister = (uint16_t*)(pOutputBuffer + OUTPUT_FCDATA1_OFFSET);
        *pOutputRegister = htons(unRegister);
        uint16_t* pOutputCount = (uint16_t*)(pOutputBuffer + OUTPUT_FCDATA2_OFFSET);
        *pOutputCount = htons(unCount);
        *pOutputLen = FC05_LEN;
        nErrorCode = NO_ERRORS;
    }

    return nErrorCode;
}

static uint8_t DecodeModbusData(uint8_t* pInputData, uint8_t unInputSize, uint8_t* pOutputBuffer)
{
    uint8_t nErrorCode = NO_ERRORS;
    uint8_t OutputLen = 0;

    if (pInputData[ADDR_OFFSET] == MODBUS_ADDR)
    {
        uint16_t* pCRC = (uint16_t*)(pInputData + unInputSize - sizeof(uint16_t));

        if (CRC16(pInputData, unInputSize - sizeof(uint16_t)) == ntohs(*pCRC))
        {
            pOutputBuffer[ADDR_OFFSET] = MODBUS_ADDR;
            pOutputBuffer[FUNC_OFFSET] = pInputData[FUNC_OFFSET];
            uint16_t* pRegister = (uint16_t*)(pInputData + DATA_OFFSET);
            uint16_t unRegister = htons(*pRegister);
            uint16_t* pCount = (uint16_t*)(pInputData + COUNT_OFFSET);
            uint16_t unCount = ntohs(*pCount);

            switch (pInputData[FUNC_OFFSET])
            {
            case READ_COIL_STATUS:
                nErrorCode = ReadCoilStatus(unRegister, unCount, pOutputBuffer, &OutputLen);
                break;

            case READ_INPUT_STATUS:
                nErrorCode = ReadInputStatus(unRegister, unCount, pOutputBuffer, &OutputLen);
                break;

            case READ_INPUT_REGISTERS:
                nErrorCode = ReadInputRegisters(unRegister, unCount, pOutputBuffer, &OutputLen);
                break;

            case FORCE_SINGLE_COIL:
                nErrorCode = ForceSingeCoil(unRegister, unCount, pOutputBuffer, &OutputLen);
                break;

            case FORCE_MULTIPLE_COILS:
                nErrorCode = ForceMultipleCoils(unRegister, unCount, pInputData[FC_COILS_OFFSET], pOutputBuffer, &OutputLen);
                break;

            default:
                nErrorCode = ILLEGAL_FUNCTION;
            }

            if (NO_ERRORS != nErrorCode)
            {
                pOutputBuffer[FUNC_OFFSET] |= ERROR_FLAG;
                pOutputBuffer[DATA_OFFSET] = nErrorCode;
                OutputLen = HEADER_LEN;
            }

            pCRC = (uint16_t*)(pOutputBuffer + OutputLen);
            *pCRC = htons(CRC16(pOutputBuffer, OutputLen));
            OutputLen += sizeof(uint16_t);
        }
    }

    return OutputLen;
}

static void OnModbusTimer()
{
    TimerModbus_Stop();
    g_nStatus |= EVENT_MODBUS;
}

static void OnModbusReceiveByte(uint8_t unData)
{
    if (stUartData.nBytesCount < UART_BUFFER_SIZE)
    {
        stUartData.InputBuffer[stUartData.nBytesCount++] = unData;
    }

    TimerModbus_Start();
}

void ModbusInit()
{
    memset(&stUartData, 0, sizeof(stUartData));
    memset(&stModbusData, 0, sizeof(stModbusData));

    TimerModbus_Init(&OnModbusTimer);
    UartInit(&OnModbusReceiveByte);
    UartStart();
}

void SetInputRegister(uint8_t unRegister, int16_t nValue)
{
    if (unRegister < MODBUS_INPUTREG_COUNT)
    {
        stModbusData.nInputRegisters[unRegister] = nValue;
    }
}

void SetDiscreteInput(uint8_t unInput, uint8_t unValue)
{
    if (unInput < MODBUS_INPUTS_COUNT)
    {
        stModbusData.unInputs[unInput] = unValue;
    }
}

uint8_t GetCoilValue(uint8_t unCoil)
{
    uint8_t Result = 0;

    if (unCoil < MODBUS_COILS_COUNT)
    {
        Result = stModbusData.unCoils[unCoil];
    }

    return Result;
}

void ModbusHandler()
{
    uint8_t pOutputBuffer[OUTPUT_BUFFER_SIZE];
    uint8_t OutputSize = DecodeModbusData(stUartData.InputBuffer, stUartData.nBytesCount, pOutputBuffer);
    stUartData.nBytesCount = 0;
    UartStart();
    UartWrite(pOutputBuffer, OutputSize);
}