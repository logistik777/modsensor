#include "sensordata.h"
#include "mysqldata.h"
#include <stdio.h>

#define SQL_MAXLEN  256

static const int RESP_TIMEOUT = 2;  // 2 sec timeout


int ConnectSensor(modbus_t *pModbusCtx)
{
    modbus_set_slave(pModbusCtx, 1);
    modbus_set_response_timeout(pModbusCtx, RESP_TIMEOUT, 0);
    modbus_set_byte_timeout(pModbusCtx, 0, 0);
    return modbus_connect(pModbusCtx);
}

int FillRegistersData(modbus_t *pModbusCtx, MYSQL *pConnection)
{
    int Result = RES_ERROR;
    MYSQL_RES* pOpenResult = OpenRegistersTable(pConnection);
    if(pOpenResult != NULL)
    {
        int nModbusRegister = 0;
        int16_t sRegValue = 0;
        while((nModbusRegister = GetNextRecord(pConnection,pOpenResult))>=0)
        {
            int nModbusResult = modbus_read_input_registers(pModbusCtx, nModbusRegister, 1, &sRegValue);
            if(nModbusResult == RES_ERROR)
            {
                break;
            }
            char strQuery[SQL_MAXLEN];
            sprintf(strQuery,"CALL UpdateModbusRegisterValue('%d', '%d')", nModbusRegister, sRegValue);
            if (mysql_query(pConnection, strQuery)<0) 
            {
                break;
            }
        }
        mysql_free_result(pOpenResult);
        Result = RES_OK;
    }
    return Result;
}

int FillInputsData(modbus_t *pModbusCtx, MYSQL *pConnection)
{
    int Result = RES_ERROR;
    MYSQL_RES* pOpenResult = OpenInputsTable(pConnection);
    if(pOpenResult != NULL)
    {
        int nModbusInput = 0;
        uint8_t nInputValue = 0;
        while((nModbusInput = GetNextRecord(pConnection,pOpenResult))>=0)
        {
            int nModbusResult = modbus_read_input_bits(pModbusCtx, nModbusInput, 1, &nInputValue);
            if(nModbusResult == RES_ERROR)
            {
                break;
            }
            char strQuery[SQL_MAXLEN];
            sprintf(strQuery, "CALL UpdateModbusInputValue('%d', b'%d')", nModbusInput, nInputValue);
            if (mysql_query(pConnection, strQuery)<0) 
            {
                break;
            }
        }
        mysql_free_result(pOpenResult);
        Result = RES_OK;
    }
    return Result;
}

int UpdateCurrentState(MYSQL *pConnection)
{
    int Result = RES_ERROR;
    const char* strQuery = "CALL UpdateCurrentState()";
    if(mysql_query(pConnection, strQuery)>=0)
    {
        Result = RES_OK;
    }
    return Result;
}

int UpdateCoilsState(modbus_t *pModbusCtx, MYSQL *pConnection)
{
    int Result = RES_ERROR;
    MYSQL_RES* pOpenResult = OpenCoilsTable(pConnection);
    if(pOpenResult)
    {
        int nModbusCoil;
        int nCoilValue = 0;
        while((nModbusCoil = GetNextCoil(pConnection,pOpenResult, &nCoilValue))>=0)
        {
            int nModbusResult = modbus_write_bit(pModbusCtx, nModbusCoil, nCoilValue);
            if(nModbusResult == RES_ERROR)
            {
                break;
            }
        }
        mysql_free_result(pOpenResult);
        Result = RES_OK;
    }
    return Result;
}

