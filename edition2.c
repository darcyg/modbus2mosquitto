/*
 *Date	：2018.1.5
 *Author: 王灏楠
 *
*/
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include<pthread.h>
#include <mosquitto.h>
#include<modbus.h>

//#include "client_shared.h" //mosq_config的定义在这里

pthread_mutex_t mutex;
//******************************MQTT配置信息***********************************************************************************************
typedef enum { ASCII, UTF_8 }ENCODE;
typedef struct
{
	char		Remote_IP[15];
	int		    Remote_Port;
	char		Project_name[10];
	char		UniCode[25];
	int			Compress;
	int			Encrypt;
	int  		Encoding;
	int			KeepAlive;
	int			PushTime;
}MqttConfig;//  针对公司要求
#define MqttConfig_initializer {"116.62.113.69",1883,"LD","0-12345-678901-23456",0,0,1,120,1}
MqttConfig Mqtt = MqttConfig_initializer;

//struct mosq_config {
//	char *id;
//	char *id_prefix;
//	int protocol_version;
//	int keepalive;
//	char *host;
//	int port;
//	int qos;
//	bool retain;
//	int pub_mode; /* pub */
//	char *file_input; /* pub */
//	char *message; /* pub */
//	long msglen; /* pub */
//	char *topic; /* pub */
//	char *bind_address;

//struct mosq_config cfg; // mosquitto的config  另外还有一个mosquitto_message; 相当于一共3个重要的结构体
						  //以后再考虑加进来吧

//MQTT相关
int noAckTime = 5;		//无应答判别时间,单位S
int reConnectTime = 20 * 60;//连接重试间隔时间,单位S

int connectSuccessFlag = 0;//连接成功标志 默认不成功
int reConnectFlag = 0;//重连标志

int firstGetFinishFlag = 0; //第一次采集完成标志
int firstPubFlag = 1;//第一次推送标志

char PAYLOAD_A[7]; //A消息体的结构，具体见buildMessage_A函数
char message_C[100000];
//unsigned char message_H[100000];
char message_H[100000];
int recevidMessage_B = 0;//收到消息B标志 0表示没有收到
int recevidMessage_D = 0;
int recevidMessage_T = 0;
int recevidMessage_L = 0;
static int run = -1;
static int sent_mid;
struct mosquitto *mosq;

//MQTT消息体
char FRAMETYPE = 0;//帧类别
char CTRL[2];	//控制字;

//**************************modbus配置信息****************************************************************************************
typedef enum { None, Odd, Even }PARITY;
typedef struct
{
	int			SlaveID;
	char		Modbus_Serial_Type[10];
	char		Com_Port[20];
	int			Baud_Rate;
	int			Data_bits;
	char		Parity;
	int			Stop_bits;
}ModbusConfig;
#define ModbusConfig_initializer {1,"RTU","/dev/ttyUSB0",9600,8,'N',1}
ModbusConfig Modbus = ModbusConfig_initializer;

#define CONFIGLENGTH 1500
int Version;//版本信息

typedef enum {
	coil = 1,
	input,
	holdingRegister,
	inputRegister
}function;//功能码 与配置表中ModbusDateType对应

typedef struct
{
	char name[10];
	int DataType;
	int ModBusDataType;
	int ModbusAddr;
	char DeviceDateType[10];
	int DeviceDateLen;
	int num;
	char desc[20];
	int ConvertionType;
}Tab;//变量表
Tab valueTab[CONFIGLENGTH];//配置文件中的变量总表
int SumCount = 0;//
Tab	HoldregisterTab[CONFIGLENGTH];//变量总表->保持寄存器表
int SumCount_hold = 0;
Tab	InputTab[CONFIGLENGTH];//变量总表->输入线圈
int SumCount_input = 0;
int count;// 变量数据体 计数

//存储PLC中数据的变量表 modbus相关
typedef struct
{
	int num;
	int length;
	unsigned int value[2];
	int isChange;
}List;
List getData[1000]; //采集到变量存放
List cmpData[1000];//判断数据采集到信息与上次的值发生变化

//***********************************零碎小函数*******************************************************************************

void swap(int *a, int *b)
{
	int     c;
	c = *a;
	*a = *b;
	*b = c;
}


void buildCtrl(void)
{
	CTRL[1] = 0;//控制字高8位 预留
	CTRL[0] = 0;//控制字低8位 加密2 压缩格式2 编码2 格式2
	if (Mqtt.Encrypt == 1)
		CTRL[0] += 64;
	if (Mqtt.Compress == 1)
		CTRL[0] += 16;
	if (Mqtt.Encoding == 1)
		CTRL[0] += 4;
}
void buildWaitTab_D(void)
{
	pthread_mutex_lock(&mutex);//cmpData是临界资源，先上个锁
	int i, k;

	if (firstPubFlag)
	{
		for (i = 0; i<SumCount; i++)
		{
			cmpData[i].num = getData[i].num;
			cmpData[i].length = getData[i].length;
			cmpData[i].isChange = 1; //第一次全推送，相对而言全部数据发生变化
			for (k = 0; k<cmpData[i].length; k++)
				cmpData[i].value[k] = getData[i].value[k];
		}
	}
	else 
	{
		for (i = 0; i<SumCount; i++)
		{
			cmpData[i].num = getData[i].num;//考虑删掉
			cmpData[i].length = getData[i].length;
			if (memcmp(&(cmpData[i].value), &(getData[i].value), sizeof(unsigned int)*(cmpData[i].length)))//与上次采集的数据不一样
			{
				cmpData[i].isChange += 1;// isChange是只有0和1的变化吗？是的，在builidMessageH()函数中，有将isChange重新置0
				//将发生变化的数据存入等待上传表
				for (k = 0; k<cmpData[i].length; k++)
					cmpData[i].value[k] = getData[i].value[k];
			}
			else
			{
				cmpData[i].isChange += 0;
			}
		}
	}
	pthread_mutex_unlock(&mutex);//走出临界区，解锁
}
void buildWaitTab_T(void)
{
	pthread_mutex_lock(&mutex);//cmpData是临界资源，先上个锁
	int i, k;

	if (firstPubFlag || recevidMessage_T)
	{
		for (i = 0; i<SumCount; i++)
		{
			cmpData[i].num = getData[i].num;
			cmpData[i].length = getData[i].length;
			cmpData[i].isChange = 1; //第一次全推送，相对而言全部数据发生变化
			for (k = 0; k<cmpData[i].length; k++)
				cmpData[i].value[k] = getData[i].value[k];
		}
	}
	else if (!recevidMessage_T)
	{
		for (i = 0; i<SumCount; i++)
		{
			cmpData[i].num = getData[i].num;
			cmpData[i].length = getData[i].length;
			if (memcmp(&(cmpData[i].value), &(getData[i].value), sizeof(unsigned int)*(cmpData[i].length)))//与上次采集的数据不一样
			{
				cmpData[i].isChange += 1;// isChange是只有0和1的变化吗？是的，在builidMessageH()函数中，有将isChange重新置0
				//将发生变化的数据存入等待上传表
				for (k = 0; k<cmpData[i].length; k++)
					cmpData[i].value[k] = getData[i].value[k];
			}
			else
			{
				cmpData[i].isChange += 0;
			}
		}
	}
	pthread_mutex_unlock(&mutex);//走出临界区，解锁
}
int sortConfig(void)
{
	int i = 0;
	int j = 0, k = 0;
	int jj, kk;
	int testA;
	while (i<SumCount)
	{
		if (valueTab[i].ModBusDataType == holdingRegister) //判断变量表中第i行数据的功能码，若是3号功能，将该行的数据赋给HoldregisterTab变量
		{
			HoldregisterTab[j].num = valueTab[i].num;
			HoldregisterTab[j].ModBusDataType = valueTab[i].ModBusDataType;
			HoldregisterTab[j].ModbusAddr = valueTab[i].ModbusAddr;
			HoldregisterTab[j].DeviceDateLen = valueTab[i].DeviceDateLen;
			HoldregisterTab[j].DataType = valueTab[i].DataType;
			if (j>0)									 //冒泡排序
			for (jj = 0; jj<j; jj++)
			for (kk = 0; kk<j - jj; kk++)
			if (HoldregisterTab[kk].ModbusAddr>HoldregisterTab[kk + 1].ModbusAddr)
			{
				swap(&HoldregisterTab[kk].ModbusAddr, &HoldregisterTab[kk + 1].ModbusAddr);        //？？？这是把序号、地址、数据长度按照地址大小进行了排序，但是为什么不是整个结构体都排序呢？
				swap(&HoldregisterTab[kk].num, &HoldregisterTab[kk + 1].num);
				swap(&HoldregisterTab[kk].DeviceDateLen, &HoldregisterTab[kk + 1].DeviceDateLen);
			}
			j++;
		}
		else if (valueTab[i].ModBusDataType == input)
		{
			InputTab[k].num = valueTab[i].num;
			InputTab[k].ModBusDataType = valueTab[i].ModBusDataType;
			InputTab[k].ModbusAddr = valueTab[i].ModbusAddr;
			InputTab[k].DeviceDateLen = valueTab[i].DeviceDateLen;
			InputTab[k].DataType = valueTab[i].DataType;
			if (k>1)
			for (jj = 0; jj<j; jj++)
			for (kk = 0; kk<k - jj; kk++)
			if (InputTab[kk].ModbusAddr>InputTab[kk + 1].ModbusAddr)
			{
				swap(&InputTab[kk].num, &InputTab[kk + 1].num);
				swap(&InputTab[kk].ModbusAddr, &InputTab[kk + 1].ModbusAddr);
				swap(&InputTab[kk].DeviceDateLen, &InputTab[kk + 1].DeviceDateLen);
			}
			k++;
		}
		else
		{
			printf("其他功能码暂时未实现\n");
		}
		i++;
	}
	SumCount_hold = j;
	SumCount_input = k;
	printf("\nSumCount_hold = %d SumCount_input= %d\n", SumCount_hold, SumCount_input);
	printf("\n===============================================\n");
	printf("FunctionCode num ModbusAddr DeviceDateLen DateType\n");
	for (testA = 0; testA<SumCount_hold; testA++)
	{
		printf("Holdregister>>>%3d %4d %5d%3d\n", HoldregisterTab[testA].num, HoldregisterTab[testA].ModbusAddr, HoldregisterTab[testA].DeviceDateLen, HoldregisterTab[testA].DataType);
	}
	printf("===============================================\n");
	for (testA = 0; testA<SumCount_input; testA++)
	{
		printf("       input>>>%3d %4d %5d%3d\n", InputTab[testA].num, InputTab[testA].ModbusAddr, InputTab[testA].DeviceDateLen, InputTab[testA].DataType);
	}
	printf("===============================================\n");
	return 0;
}

#define MAX_LINE_LEN 1000
int readConfig(void)
{
	char line[MAX_LINE_LEN] = { 0 };   //临时存放配置信息的
	FILE *fp = fopen("config.txt", "r");
	int tempCount = 0;
	if (fp == NULL)
	{
		printf("Fail to open file config.txt, %s.\n", strerror(errno));
		return 0;
	}
	printf("读取配置文件\n\n");
	while (fgets(line, MAX_LINE_LEN + 1, fp) != NULL)
	{
		if (tempCount == 1)
		{
			fscanf(fp, "%d%s%s%d%d%s%d", &Modbus.SlaveID, &Modbus.Modbus_Serial_Type, &Modbus.Com_Port, &Modbus.Baud_Rate, &Modbus.Data_bits, &Modbus.Parity, &Modbus.Stop_bits);
			printf("\n%d	%s	%s	%d	%d	%c	%d\n", Modbus.SlaveID, Modbus.Modbus_Serial_Type, Modbus.Com_Port, Modbus.Baud_Rate, Modbus.Data_bits, Modbus.Parity, Modbus.Stop_bits);
		}
		else if (tempCount == 5)
		{
			fscanf(fp, "%s%d%s%s%d%d%d%d%d", &Mqtt.Remote_IP, &Mqtt.Remote_Port, &Mqtt.Project_name, &Mqtt.UniCode, &Mqtt.Compress, &Mqtt.Encrypt, &Mqtt.Encoding, &Mqtt.KeepAlive, &Mqtt.PushTime);
			printf("\n%s	%d	%s	%s	%d	%d	%d	%d	%d\n", Mqtt.Remote_IP, Mqtt.Remote_Port, Mqtt.Project_name, Mqtt.UniCode, Mqtt.Compress, Mqtt.Encrypt, Mqtt.Encoding, Mqtt.KeepAlive, Mqtt.PushTime);
		}
		else if (tempCount == 9)
		{
			fscanf(fp, "%d", &Version);
			printf("\n===version=%d\n\n", Version);
		}
		else if (tempCount == 13)
		{
			int count = 0;
			while (-1 != fscanf(fp, "%s%d%d%d%s%d%d%s%d", &valueTab[count].name, &valueTab[count].DataType, &valueTab[count].ModBusDataType, &valueTab[count].ModbusAddr, &valueTab[count].DeviceDateType, &valueTab[count].DeviceDateLen, &valueTab[count].num, &valueTab[count].desc, &valueTab[count].ConvertionType))
			{
				printf("%8s	%d	%d	%d	%s	%d	%d	%s	%d\n", valueTab[count].name, valueTab[count].DataType, valueTab[count].ModBusDataType, valueTab[count].ModbusAddr, valueTab[count].DeviceDateType, valueTab[count].DeviceDateLen, valueTab[count].num, valueTab[count].desc, valueTab[count].ConvertionType);
				count++;
			}
			SumCount = count;
			printf("\n****SumCount %d\n\n", SumCount);
		}
		tempCount++;

	}

	if (fclose(fp) != 0)
		printf("close file Failed!");

	sortConfig();//排序
	buildCtrl();//生成控制字
	return 1;
}

//时间等待 (ms)
int MySleep(long milliseconds)
{
#if defined(WIN32) || defined(WIN64)
	Sleep(milliseconds);
#else
	usleep(milliseconds * 1000);
#endif
}

//***********************************采集modbus数据******************************************************************************  

int getModbusData(void *arg)
{
	modbus_t *ctx;
	int rc;

	//要采集的modbus 数据
	uint16_t ADDRESS;//地址
	uint16_t NB;//采集数量--变量长度
	//返回值
	uint16_t *tab_rp_registers;//字
	uint8_t *tab_rp_bits;//位

	if (Modbus.Modbus_Serial_Type[0] == 'R')//RTU
	{
		printf("getMogbusData>>> RTU\n");
		ctx = modbus_new_rtu(Modbus.Com_Port, Modbus.Baud_Rate, Modbus.Parity, Modbus.Data_bits, Modbus.Stop_bits);//N E O
	}
	else
	{
		printf("getMogbusData>>> ASCII is not achieved\n");  //待修改，库文件中是有ascii的，不知道为什么编译时有undeclare
		//ctx = modbus_new_ascii(Modbus.Com_Port, Modbus.Baud_Rate, Modbus.Parity, Modbus.Data_bits, Modbus.Stop_bits);//N E O
	}
	if (ctx == NULL)
	{
		printf("Unable to allocate libmodbus context\n");
		return -1;
	}
	//modbus_set_debug(ctx, FALSE);
	modbus_set_debug(ctx, TRUE);
	modbus_set_slave(ctx, Modbus.SlaveID);
	if (-1 == modbus_connect(ctx))
	{
		printf("Connect failed!!!\n");
		modbus_free(ctx);
		return -1;
	}

	while (1) //一直采集
	{
		int M, N;//采集变量个数 / 125 的商和余数
		int m;
		int k = 0, l = 0;//采集变量表的个数增加
		MySleep(5000);//采集周期

		if (SumCount_hold>0)//
		{	//printf(">>>读保持寄存器\n");
			ADDRESS = HoldregisterTab[0].ModbusAddr;
			NB = HoldregisterTab[SumCount_hold - 1].ModbusAddr - HoldregisterTab[0].ModbusAddr + HoldregisterTab[SumCount_hold - 1].DeviceDateLen;//最后一个的地址 - 起始地址 + 最后一个的读取长度
			M = NB / 120;
			N = NB % 120;
			for (m = 0; m<M; m++)
			{
	
				tab_rp_registers = (uint16_t *)malloc(120 * sizeof(uint16_t));
				memset(tab_rp_registers, 0, 120 * sizeof(uint16_t));
				//printf("%d modbus_read_registers %d %d \n",m+1,ADDRESS+m*125,125);

				rc = modbus_read_registers(ctx, ADDRESS + m * 120, 120, tab_rp_registers);
				/*if(rc==125)*/
				MySleep(1000);
				if (rc == 120)
				{
					int i, j;

					/*while(HoldregisterTab[k].ModbusAddr<(ADDRESS+(m+1)*125))*/
					while (HoldregisterTab[k].ModbusAddr<(ADDRESS + (m + 1) * 120))
					{
						/*j = (HoldregisterTab[k].ModbusAddr - HoldregisterTab[0].ModbusAddr)%125;*/
						j = (HoldregisterTab[k].ModbusAddr - HoldregisterTab[0].ModbusAddr) % 120;
						getData[k].num = HoldregisterTab[k].num;
						getData[k].length = HoldregisterTab[k].DeviceDateLen;
						//下面可添加字节序调整
						if (getData[k].length == 1)
							getData[k].value[0] = tab_rp_registers[j];//与buildMessage_H  859行对应
						if (getData[k].length == 2)
						{
							//getData[k].value[0]=tab_rp_registers[j];
							//getData[k].value[1]=tab_rp_registers[j+1];
							getData[k].value[1] = tab_rp_registers[j];
							getData[k].value[0] = tab_rp_registers[j + 1];
						}
						//printf(">>>>%d >>%d\n",getData[k].value[0],getData[k].value[1]);
						k++;
					}
					/*	for(i=0;i<125;i++)
					{
					printf("%d ",tab_rp_registers[i]);
					}
					printf("\n");*/
					//printf("*********%d\n",m);
				}
				else
					printf("hold %d 120 failed!\n", m);
			}
			if (N != 0)
			{
				tab_rp_registers = (uint16_t *)malloc((NB - (M * 120)) * sizeof(uint16_t));
				memset(tab_rp_registers, 0, (NB - (M * 120)) * sizeof(uint16_t));

				rc = modbus_read_registers(ctx, ADDRESS + (M * 120), (NB - (M * 120)), tab_rp_registers);
				if (rc == (NB - (M * 120)))
				{
					int i, j;
					while (HoldregisterTab[k].ModbusAddr<ADDRESS + ((M + 1) * 120) && k<SumCount_hold)
					{
						j = (HoldregisterTab[k].ModbusAddr - HoldregisterTab[0].ModbusAddr) % 120;
						getData[k].num = HoldregisterTab[k].num;
						getData[k].length = HoldregisterTab[k].DeviceDateLen;
						if (getData[k].length == 1)
							getData[k].value[0] = tab_rp_registers[j];
						if (getData[k].length == 2)
						{
							getData[k].value[0] = tab_rp_registers[j];
							getData[k].value[1] = tab_rp_registers[j + 1];
						}
						//printf(">>>>%d >>%d\n",getData[k].value[0],getData[k].value[1]);
						k++;
					}

					/*for(i=0;i<(NB-(M*125));i++)
					printf("%d ",tab_rp_registers[i]);*/
					//printf("\n");
				}
				else
					printf("hold  NB-120 failed!\n");
			}

		}
		MySleep(1000);
		if (SumCount_input>0)
		{
			//printf(">>>读输入状态\n");
			ADDRESS = InputTab[0].ModbusAddr;
			NB = InputTab[SumCount_input - 1].ModbusAddr - InputTab[0].ModbusAddr + InputTab[SumCount_input - 1].DeviceDateLen;//最后一个的地址 - 起始地址 + 最后一个的读取长度

			M = NB / 125;
			N = NB % 125;

			for (m = 0; m<M; m++)
			{
				tab_rp_bits = (uint8_t *)malloc(125 * sizeof(uint8_t));
				memset(tab_rp_bits, 0, 125 * sizeof(uint8_t));
				rc = modbus_read_input_bits(ctx, ADDRESS + m * 125, 125, tab_rp_bits);
				if (rc == 125)
				{
					int i, j;

					while (InputTab[l].ModbusAddr<(ADDRESS + (m + 1) * 125))
					{
						j = (InputTab[l].ModbusAddr - InputTab[0].ModbusAddr) % 125;
						getData[l + SumCount_hold].num = InputTab[l].num;
						getData[l + SumCount_hold].length = InputTab[l].DeviceDateLen;
						if (getData[l + SumCount_hold].length == 1)
							getData[l + SumCount_hold].value[0] = tab_rp_bits[j];
						/*if(getData[k].length ==2)
						{
						getData[k].value[0]=tab_rp_bits[j];
						getData[k].value[1]=tab_rp_bits[j+1];
						}*/
						//printf(">>>>%d \n",getData[k].value[0]);
						l++;
					}

					/*for(i=0;i<125;i++)
					printf("%d ",tab_rp_bits[i]);
					printf("\n");*/
				}
				else
					printf("input failed!\n");
			}
			if (N != 0)
			{
				tab_rp_bits = (uint8_t *)malloc((NB - (M * 125)) * sizeof(uint8_t));
				memset(tab_rp_bits, 0, (NB - (M * 125)) * sizeof(uint8_t));
				rc = modbus_read_input_bits(ctx, ADDRESS + (M * 125), (NB - (M * 125)), tab_rp_bits);

				if (rc == (NB - (M * 125)))
				{
					int i, j;

					while (InputTab[l].ModbusAddr<ADDRESS + ((M + 1) * 125) && l<SumCount_input)
					{
						j = (InputTab[l].ModbusAddr - InputTab[0].ModbusAddr) % 125;
						getData[l + SumCount_hold].num = InputTab[l].num;
						getData[l + SumCount_hold].length = InputTab[l].DeviceDateLen;
						if (getData[l + SumCount_hold].length == 1)
							getData[l + SumCount_hold].value[0] = tab_rp_bits[j];
						/*if(getData[k].length ==2)
						{
						getData[k].value[0]=tab_rp_bits[j];
						getData[k].value[1]=tab_rp_bits[j+1];
						}*/
						//printf(">>>>%d \n",getData[l+SumCount_hold].value[0]);
						l++;
					}
					/*for(i=0;i<(NB-(M*125));i++)
					printf("%d ",tab_rp_bits[i]);
					printf("\n");*/
				}
				else
					printf("input failed!\n");
			}
		}
		//break;//test
		//printf("\n#############################################################\n");
		//TODO
		firstGetFinishFlag = 1;//第一次采采集完成
		buildWaitTab_T();
	}
	return 1;
}


// ******************************** 构建 消息内容 和 发送消息 函数****************************************************************
void buildMessage_A(char frameType, char ctrl[2], int data)
{
	int i;

	PAYLOAD_A[0] = frameType;
	PAYLOAD_A[1] = ctrl[1];
	PAYLOAD_A[2] = ctrl[0] + 0;//只有消息C 为JSon  需要 +1

	if (Mqtt.Encrypt || Mqtt.Encoding || Mqtt.Compress)
	{
		//data进行相应加密压缩编码处理
	}
	PAYLOAD_A[3] = data >> 24;
	PAYLOAD_A[4] = (data >> 16) & 0x00FF;
	PAYLOAD_A[5] = (data >> 8) & 0x0000FF;
	PAYLOAD_A[6] = data & 0xFF;

	/*for(i=0;i<7;i++)
	printf("%d ",rc[i]);
	printf("\n");*/
	//return rc;
}
void sendMessage_A(void)
{
	FRAMETYPE = 0x01;//帧类别
	buildCtrl();//生成控制字
	buildMessage_A(FRAMETYPE, CTRL, Version);
	struct mosquitto_message pubmsgA;
	pubmsgA.payload = PAYLOAD_A;
	pubmsgA.payloadlen = sizeof(PAYLOAD_A);
	pubmsgA.qos = 1;
	pubmsgA.retain = 0;//服务器端必须存储这个应用消息和它的服务质量等级（qos），以便它可以被分发给未来的主题名匹配的订阅者
	
	int i;
	printf("This is messageA:\n");
	for (i = 0; i < 7; i++)
	{
		printf("%x\n", PAYLOAD_A[i]);
	}

	mosquitto_publish(mosq, &sent_mid, "LD/TTTTTT/P/A", pubmsgA.payloadlen, pubmsgA.payload, pubmsgA.qos, pubmsgA.retain);

}

//构建上传数据 C
char* buildJson(void)
{
	int i;
	int offsetC = 0;
	//char tempC[1000];//消息C 临时存放量 用来生成Json		<<<< buildJson
	char* tempC;
	tempC = (char*)malloc(sizeof(char)*(100 * SumCount + 29));//29为Json上下行的字节数，100为每行的字节数
	offsetC += sprintf(tempC, "{\"ver\":%d,\"taglst\":[", Version);
	for (i = 0; i<SumCount; i++)
		offsetC += sprintf(tempC + offsetC, "{\"nam\":\"%s\",\"dsc\":\"%s\",\"addr\":%d,\"vt\":%d},", valueTab[i].name, valueTab[i].desc, valueTab[i].num, valueTab[i].DataType);
	offsetC += sprintf(tempC + offsetC - 1, "]}");
	return tempC;
}
char* buildMessage_C(char frameType, char ctrl[2], int* length)
{
	char* pointTab = buildJson();
	int i;
	message_C[0] = frameType;
	message_C[1] = ctrl[1];//保证 strcat
	message_C[2] = ctrl[0] + 1;//只有消息 C +1  为 json

	if (Mqtt.Encrypt || Mqtt.Encoding || Mqtt.Compress)
	{
		//pointTab进行相应加密压缩编码处理
	}
	/*printf("*****frameType*****%d\n",message_C[0]);
	printf("*****ctrl 1*****%d\n",message_C[1]);
	printf("*****ctrl 0*****%d\n",message_C[2]);
	printf("****strlen(pointTab)= %d\n",strlen(pointTab));*/
	*length = strlen(pointTab) + 3;//为什么要加一个3？
	for (i = 0; i<strlen(pointTab); i++)
		message_C[3 + i] = pointTab[i];

	return message_C;
}
char* buildMessage_H(char frameType, char ctrl[2], int* length, int *countReturn)
{
	int j = 0;//message_H 数组下标
	int i;//sumCount 计数
	int k;//数据值  根据变量类型 长度计数

	int upLoadDataType;
	int testA;
	unsigned int t = time(NULL);
	count = 0;// 变量数据体 计数
	message_H[j++] = frameType;//帧类型
	message_H[j++] = ctrl[1];
	message_H[j++] = ctrl[0] + 0;//只有消息 C +1  为 json

	/*for(i=0;i<SumCount;i++)
	{
	printf("num = %d length = %d	value:",getData[i].num,getData[i].length);
	for(j=0;j<getData[i].length;j++)
	printf("%d	",getData[i].value[j]);
	printf("\n");
	}*/

	if (Mqtt.Encrypt)//基本用不到 预留的接口
	{	//加密前的长度 UINT32
		message_H[j++] = 1;
		message_H[j++] = 1;
		message_H[j++] = 1;
		message_H[j++] = 1;
	}
	if (Mqtt.Compress)//基本用不到 预留的接口
	{//压缩前的长度UINT32
		message_H[j++] = 1;
		message_H[j++] = 1;
		message_H[j++] = 1;
		message_H[j++] = 1;
	}
	//UTC秒
	message_H[j++] = t >> 24;					//UTC时间 第一个字节
	message_H[j++] = (t >> 16) & 0x00FF;		//UTC时间 第二个字节
	message_H[j++] = (t >> 8) & 0x0000FF;		//UTC时间 第三个字节
	message_H[j++] = t & 0xFF;				//UTC时间 第四个字节 //是否应该是&0x000000FF

	message_H[j++] = 0;					// //数据体个数高8位 先设置为0 message_H[7]
	message_H[j++] = 0;					// //数据体个数低8位 先设置为0 message_H[8]  在统计完重新赋值

	for (i = 0; i<SumCount; i++)
	{
		pthread_mutex_lock(&mutex);//进入临界资源区，cmpData是临界资源，上锁****这个锁加在for循环外面会不会更好？？？
		if (cmpData[i].isChange != 0)
		{
			cmpData[i].isChange = 0;//清除 改变标志
			//变量数据体
			message_H[j++] = cmpData[i].num / 0xff;//变量序号高8位，相当于向右平移8位，不过这里num是个4字节的int，这样不对吧。公司的需求是UINT16的，是不是要改成short int。但是又考虑到num如果不大于256的话，也不会出问题，那就先这样。
			message_H[j++] = cmpData[i].num % 0xff;//变量序号低8位

			switch (valueTab[cmpData[i].num - 1].DataType)
			{
			case 1:upLoadDataType = 1; break; //1字节整型
			case 2:							//2字节有符号整型
			case 3:upLoadDataType = 2; break;//2字节无符号整型
			case 4:							//4字节有符号整型
			case 5:							//4字节无符号整型
			case 6:upLoadDataType = 4; break;//4字节float
			case 7:upLoadDataType = 8; break;//8字节double
				//default:upLoadDataType = valueTab[cmpData[i].num-1].DataType-128; //String：最高位为1代表string，后7位代表字符串 长度
			default:upLoadDataType = 3;
			}
			//upLoadDataType = valueTab[cmpData[i].num-1].DataType; 
			//message_H[j++] = upLoadDataType & 0xFF;//变量类型
			if (cmpData[i].num - 1>80)//这句话应该是仅仅针对变量表是82个变量而来的。
				printf("%d %d\n", cmpData[i].num, valueTab[cmpData[i].num].DataType);//不知道这是干什么？
			message_H[j++] = valueTab[cmpData[i].num - 1].DataType;//变量类型

			for (k = 0; k<upLoadDataType; k++) // DeviceDateLen -->DateType  //这是在写变量数据，但看不懂这是怎么操作的？
			{
				if (valueTab[cmpData[i].num - 1].DeviceDateLen == 2)
				{
					if (k == (upLoadDataType / 2 - 2))
						message_H[j++] = cmpData[i].value[0] >> 8;
					else if (k == (upLoadDataType / 2 - 1))
						message_H[j++] = cmpData[i].value[0];
					else if (k == (upLoadDataType - 2))
						message_H[j++] = cmpData[i].value[1] >> 8;
					else if (k == (upLoadDataType - 1))
						message_H[j++] = cmpData[i].value[1];
					else
						message_H[j++] = 0;
				}
				else
				{
					if (k == (upLoadDataType - 2))
						message_H[j++] = cmpData[i].value[0] >> 8;
					else if (k == (upLoadDataType - 1))
						message_H[j++] = cmpData[i].value[0];
					else
						message_H[j++] = 0;
				}

			}
			count++;			//变量数据体 计数
		}
		pthread_mutex_unlock(&mutex);
	}
	*countReturn = count;
	message_H[7 + (Mqtt.Encrypt * 4) + (Mqtt.Compress * 4)] = count >> 8;
	message_H[8 + (Mqtt.Encrypt * 4) + (Mqtt.Compress * 4)] = count & 0xFF;
	*length = j;
	/*for(testA=0;testA<j;testA++)
	{
	printf("<%d>",message_H[testA]);
	}
	printf("\n");	*/


	return message_H;
}
int sendMessage_C(void)
{
	struct mosquitto_message pubmsgC = { 0, NULL, NULL, 0, 0, 0 };
	char* TOPIC_C = NULL;
	TOPIC_C = (char*)malloc(sizeof(char)* 30);	//变量表发送
	if (TOPIC_C == NULL)
	{
		perror("malloc");
		return -1;
	}
	char* PAYLOAD_C;
	int length;
	sprintf(TOPIC_C, "%s/%s/P/C", Mqtt.Project_name, Mqtt.UniCode);

	FRAMETYPE = 0x02;
	PAYLOAD_C = buildMessage_C(FRAMETYPE, CTRL, &length);
	//printf("message C payload: %s\n",PAYLOAD_C);
	pubmsgC.payload = PAYLOAD_C;
	pubmsgC.payloadlen = length;
	pubmsgC.qos = 1;
	pubmsgC.retain = 0;

	printf("This is meaasgeC:\n");
	int i;
	for (i = 0; i < 3; i++)
	{
		printf("%x\n", PAYLOAD_C[i]);
	}
	for (i = 3; i < length - 3; i++)
	{
		printf("%c", PAYLOAD_C[i]);
	}

	mosquitto_publish(mosq, NULL, TOPIC_C, pubmsgC.payloadlen, pubmsgC.payload, pubmsgC.qos, pubmsgC.retain);
	free(TOPIC_C);
	return 1;
	//等待一分钟  先改为1s
	//MySleep(1000*60)		
	//	MySleep(1000);
	//mosquitto_publish(mosq, &sent_mid, "LD/TTTTTT/P/C", strlen("C"), "C", 0, false); 测试用代码
}
int sendMessage_H(void)
{
	struct mosquitto_message pubmsgH = { 0, NULL, NULL, 0, 0, 0 };
	char*TOPIC_H = NULL;
	TOPIC_H = (char*)malloc(sizeof(char)* 30);	//实时变量值上传
	if (TOPIC_H == NULL)
	{
		perror("malloc");
		return -1;
	}
	char* PAYLOAD_H;
	int length;
	int testA; 
	int countReturn = 0;

	firstPubFlag = 0;//	取消第一次推送标志
	sprintf(TOPIC_H, "%s/%s/P/H", Mqtt.Project_name, Mqtt.UniCode);

	FRAMETYPE = 0x0A;
	PAYLOAD_H = buildMessage_H(FRAMETYPE, CTRL, &length, &countReturn);//这是引用的用法吗？
	printf("count = %d \n",countReturn);
	

	if (count != 0) // 变量数据体 计数
	{
		if (recevidMessage_T && (countReturn != SumCount)) return -1;
		pubmsgH.payload = PAYLOAD_H;
		pubmsgH.payloadlen = length;

		printf("This is messageH:\n");
		for (testA = 0; testA<length; testA++)
		{
			printf("<%d>", PAYLOAD_H[testA]);
		}
		printf("\n\n");

		pubmsgH.qos = 1;
		pubmsgH.retain = 0;
		mosquitto_publish(mosq, NULL, TOPIC_H, pubmsgH.payloadlen, pubmsgH.payload, pubmsgH.qos, pubmsgH.retain);
		///*if (recevidMessage_T && (countReturn == SumCount)) {
		//	recevidMessage_T = 0;*/
		//	//printf("取消T\n");
		//}
	}
	else
	{
		//	printf("no message need to publish... \n");
	}
	free(TOPIC_H);
	return 1;
	//mosquitto_publish(mosq, &sent_mid, "LD/TTTTTT/P/H", strlen("H"), "H", 0, false); 测试用代码
}
void response_D(void)
{
	while (recevidMessage_D == 1)
	{
		buildWaitTab_D();
		sendMessage_H();
		MySleep(5000);
	}
	recevidMessage_D = 0;
}
int recMessage_download(struct mosquitto *mosq, const struct mosquitto_message *msg)
{//尝试下载一个名为test的二进制文件
	char* DownloadMsg;
	DownloadMsg = msg->payload;
	int j;
	for (j = 0 ; j < msg->payloadlen; j++)
	{
		printf("%c", DownloadMsg[j]);
	}
	int i;//接收返回值
	FILE *fp;
	if ((fp = fopen("test", "w+b")) == NULL)//w+打开可读写文件，b：二进制
	{
		printf("Can't open file!");
			exit(1);
	}
	i = fwrite(msg->payload,sizeof(char),msg->payloadlen, fp);
	printf("input nmemb is %d\n", i);
	fclose(fp);
	return 0;

}


//*****************************回调函数们**************************************************
void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
	if (rc){
		printf("connect mqtt error!!");
		exit(1);
	}
	else{
		mosquitto_subscribe(mosq, NULL,"LD/TTTTTT/S/B",0);
		mosquitto_subscribe(mosq, NULL, "LD/TTTTTT/S/D", 0);
		mosquitto_subscribe(mosq, NULL, "LD/TTTTTT/S/T", 0);
		mosquitto_subscribe(mosq, NULL, "LD/TTTTTT/S/L", 0);
		mosquitto_subscribe(mosq, NULL, "download", 0);
		sendMessage_A();
		//mosquitto_publish(mosq, &sent_mid, "LD/TTTTTT/P/A", strlen("message"), "message", 0, false); 测试用代码
	}
}
void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
	printf("sent successfully!!\n");
}

void my_message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
	if (strcmp(msg->topic,"LD/TTTTTT/S/B")==0)
	{
		recevidMessage_B = 1;
	}
	else if (strcmp(msg->topic, "LD/TTTTTT/S/D") == 0)
	{
		recevidMessage_D = 1;
	}
	else if (strcmp(msg->topic, "LD/TTTTTT/S/T") == 0)
	{
		recevidMessage_T = 1;
	}
	else if (strcmp(msg->topic, "LD/TTTTTT/S/L") == 0)
	{
		recevidMessage_L = 1;
	}
	else if (strcmp(msg->topic,"download") == 0)
	{
		recMessage_download(mosq, msg);
	}
	else
	{
		//do nothing
	}
	//mosquitto_disconnect(mosq); 
	//printf("%s\n",msg->topic);测试时加进来的
	
}

void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
	run = rc;
}
//*****************************主函数*****************************************************
int main(int argc, char *argv[])
{

	pthread_t id1,id2;
	readConfig();
	pthread_create(&id1, NULL, (void *)getModbusData, NULL);
	int rc;
	//struct mosq_config cfg;
	//cfg.username = "admin";
	//cfg.password = "password";//该用在哪里呢？

	mosquitto_lib_init();

	mosq = mosquitto_new("gateway", true, NULL);

	/*
	 *Problem:waiting to improve
	 *Explaination:如果使用下面两个函数，我理解就是使用了openssl安全协议，在连接时会服务器
	 *				端会报错，“Socket error on client<unknown>,discounting”,应该是client需要
	 *				安全注册。注释掉这两个函数就能够连接成功了，安全方面的事情现在暂时先不管。
	 */
	//mosquitto_tls_opts_set(mosq, 1, "tlsv1", NULL);  //Set advanced SSL / TLS options.Must be called before <mosquitto_connect>.
	//rc = mosquitto_tls_psk_set(mosq, "deadbeef", "psk-id", NULL);
	//if (rc){
	//	mosquitto_destroy(mosq);
	//	return rc;
	//}
	mosquitto_connect_callback_set(mosq, on_connect); //过程是这样：（1）我们调用mosquitto_connect函数，向broker请求连接
																  //（2）broker返回一个CONNACK信息
																  //（3）收到CONNACK信息后，调用该回调函数，回调函数又会调用on_connect函数。
	mosquitto_disconnect_callback_set(mosq, on_disconnect);//当broker收到DISCONNECT命令并且断开client的时候调用
	mosquitto_publish_callback_set(mosq, on_publish);   // publish完就调用这个函数

	mosquitto_message_callback_set(mosq, my_message_callback);//当broker发送信息，我们接收到的时候，就调用该函数

	rc = mosquitto_connect(mosq, Mqtt.Remote_IP, Mqtt.Remote_Port, Mqtt.KeepAlive);//公司服务器ip"116.62.113.69"
	//rc = mosquitto_connect(mosq, "192.168.1.250", 1883, 120);//公司服务器ip"116.62.113.69"
	if (rc){
		mosquitto_destroy(mosq);
		return rc;
	}

	while (!firstGetFinishFlag)
	{
		MySleep(1000);//在第一次采集完成之前，一直等待
	}

	while (run == -1){
		mosquitto_loop(mosq, -1, 1);

		if (recevidMessage_B == 1)
		{
			recevidMessage_B = 0;
			sendMessage_C();
			printf("send message C \n");

		}
		if (recevidMessage_D == 1)
		{
			pthread_create(&id2, NULL, (void *)response_D, NULL);
		}
		if (recevidMessage_T == 1)
		{
			buildWaitTab_T();
			sendMessage_H();
			printf("send message H \n");
			recevidMessage_T = 0;
		}
		//if ((recevidMessage_D == 1) || (recevidMessage_T == 1))
		//{//转发表中			实时数据
		//	buildWaitTab();
		//	recevidMessage_D = 0;
		//	recevidMessage_T = 0;
		//	sendMessage_H();
		//	printf("send message H \n");

		//}
		if (recevidMessage_L == 1)
		{
			recevidMessage_L = 0;
			//收到下行命令 暂时未处理
		}
	}

	mosquitto_destroy(mosq);

	mosquitto_lib_cleanup();
	return run;
}
//主函数还没改完
