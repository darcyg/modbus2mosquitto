#include"modbus.h"
#include<stdio.h>
#include<stdlib.h>
//#include<Windows.h>
#include<string.h>
#include <stdbool.h>
#include <stdint.h>
//#include <time.h>
#include <mosquitto.h>

#include <msgsps_common.h>

static bool run = true;
static int message_count = 0;
static struct timeval start, stop;

void my_connect_callback(struct mosquitto *mosq, void *obj, int rc)
{
	printf("rc: %d\n", rc);
	//gettimeofday(&start, NULL); windows下没有这个函数
}

void my_disconnect_callback(struct mosquitto *mosq, void *obj, int result)
{
	run = false;
}

void my_publish_callback(struct mosquitto *mosq, void *obj, int mid)
{
	message_count++;
	//printf("%d ", message_count);
	if (message_count == MESSAGE_COUNT){
	//	gettimeofday(&stop, NULL);   windows下没有这个函数
		mosquitto_disconnect((struct mosquitto *)obj);
	}
}

//int create_data(void)
//{
//	int i;
//	FILE *fptr, *rnd;
//	int rc = 0;
//	char buf[MESSAGE_SIZE];
//
//	fptr = fopen("msgsps_pub.dat", "rb");
//	if (fptr){
//		fseek(fptr, 0, SEEK_END);
//		if (ftell(fptr) >= MESSAGE_SIZE*MESSAGE_COUNT){
//			fclose(fptr);
//			return 0;
//		}
//		fclose(fptr);
//	}
//
//	fptr = fopen("msgsps_pub.dat", "wb");
//	if (!fptr) return 1;
//	rnd = fopen("/dev/urandom", "rb");
//	if (!rnd){
//		fclose(fptr);
//		return 1;
//	}
//
//	for (i = 0; i<MESSAGE_COUNT; i++){
//		if (fread(buf, sizeof(char), MESSAGE_SIZE, rnd) != MESSAGE_SIZE){
//			rc = 1;
//			break;
//		}
//		if (fwrite(buf, sizeof(char), MESSAGE_SIZE, fptr) != MESSAGE_SIZE){
//			rc = 1;
//			break;
//		}
//	}
//	fclose(rnd);
//	fclose(fptr);
//
//	return rc;
//}

int main()
{
	modbus_t *ctx;
	ctx = modbus_new_rtu("/dev/ttyS0",9600,'N',8,1);//RS485 ARM
	if(ctx==NULL)
	{
		printf("Unable to allocate libmodbus context\n");
		return -1;
	}
	modbus_set_debug(ctx,TRUE);
	int slaveID=1;
	modbus_set_slave(ctx,slaveID);

	if(-1 == modbus_connect(ctx))
	{
		printf("Connect failed!!!\n");
		modbus_free(ctx);
		return -1;
	}
	
	int rc;
	

	//保持寄存器
	const uint16_t UT_REGISTERS_ADDRESS = 10;
	const uint16_t UT_REGISTERS_NB = 0x03;
	const uint16_t UT_REGISTERS_TAB[] = { 0x022B, 0x0001, 0x0064 };
	uint16_t *tab_rp_registers;
	tab_rp_registers = (uint16_t *) malloc(UT_REGISTERS_NB * sizeof(uint16_t));
    memset(tab_rp_registers, 0, UT_REGISTERS_NB * sizeof(uint16_t)); // 看懂这一段代码，这个函数是windows下才有吗？


	//	04
	rc = modbus_read_input_registers(ctx, UT_REGISTERS_ADDRESS,1, tab_rp_registers);
    printf("2/2 modbus_read_registers: ");
    if (rc != 1) {
        printf("FAILED (nb points %d)\n", rc);
        goto close;
    }

    if (tab_rp_registers[0] != 0x1234) {
        printf("FAILED (%0X != %0X)\n",
               tab_rp_registers[0], 0x1234);
        goto close;
    }
    printf("OK\n");
	

	
close:	
	//释放内存
//    free(tab_rp_bits);
   
	//关闭连接
	modbus_close(ctx);
	modbus_free(ctx);

//**************************mosquitto程序段*********************************************
	struct mosquitto *mosq;
	int i;
	//double dstart, dstop, diff;
	//FILE *fptr;
	//uint16_t *buf;

	//buf = (uint16_t *)malloc(MESSAGE_SIZE*MESSAGE_COUNT); //分配了这么多的内存空间
	//if (!buf){
	//	printf("Error: Out of memory.\n");
	//	return 1;
	//}


	//if (create_data()){
	//	printf("Error: Unable to create random input data.\n");
	//	return 1;
	//}
	//fptr = fopen("msgsps_pub.dat", "rb");
	//if (!fptr){
	//	printf("Error: Unable to open random input data.\n");
	//	return 1;
	//}
	//fread(buf, sizeof(uint8_t), MESSAGE_SIZE*MESSAGE_COUNT, fptr);
	//fclose(fptr);

	mosquitto_lib_init();

	mosq = mosquitto_new("perftest", true, NULL);
	mosquitto_connect_callback_set(mosq, my_connect_callback);
	mosquitto_disconnect_callback_set(mosq, my_disconnect_callback);
	mosquitto_publish_callback_set(mosq, my_publish_callback);

	mosquitto_connect(mosq, "192.168.1.250", 1883, 600);

	i = 0;
	while (!mosquitto_loop(mosq, 1, 10) && run){
		if (i<MESSAGE_COUNT){
			mosquitto_publish(mosq, NULL, "perf/test", MESSAGE_SIZE, &tab_rp_registers[0], 0, false);
			i++;
		}
	}
	/*dstart = (double)start.tv_sec*1.0e6 + (double)start.tv_usec;
	dstop = (double)stop.tv_sec*1.0e6 + (double)stop.tv_usec;
	diff = (dstop - dstart) / 1.0e6;

	printf("Start: %g\nStop: %g\nDiff: %g\nMessages/s: %g\n", dstart, dstop, diff, (double)MESSAGE_COUNT / diff);
*/
	free(tab_rp_registers);
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
	return 0;
}
