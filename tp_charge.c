#include <stdint.h>
#include <sys/io.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

uint16_t smapi_port1 = 0xB2;
const uint16_t smapi_port2 = 0x4F;

int get_threshold(int bat, int start, uint8_t *val)
{
  int eax, ecx, errcode;

  asm volatile ("out %%al, %2\n\t"
		"out %%al, $0x4F"
		: "=c" (ecx), "=a" (eax)
		: "d" (smapi_port1),
		  "a" (0x5380), "b" (start ? 0x2116 : 0x211a),
		  "c" ((bat+1) << 8), "S" (0), "D" (0)
		: "cc");

  errcode = (eax >> 8) & 0xFF;
  if (errcode == 0xA6)
    return -EAGAIN;
  else if (errcode == 0x53)
    return -ENOSYS;
  else if (errcode != 0)
    return -EIO;

  if ((ecx & 0x0100) != 0x0100)
    return -EIO;

  *val = ecx & 0xFF;
  return 0;
}

int set_threshold(int bat, int start, uint8_t val)
{
  int eax, ecx, errcode;
  int esi, edi;

  /* First query, but keep ESI and EDI. */
  asm volatile ("out %%al, %4\n\t"
		"out %%al, $0x4F"
		: "=c" (ecx), "=a" (eax), "=S" (esi), "=D" (edi)
		: "d" (smapi_port1),
		  "a" (0x5380), "b" (start ? 0x2116 : 0x211a),
		  "c" ((bat+1) << 8), "S" (0), "D" (0)
		: "cc");

  errcode = (eax >> 8) & 0xFF;
  if (errcode == 0xA6)
    return -EAGAIN;
  else if (errcode == 0x53)
    return -ENOSYS;
  else if (errcode != 0)
    return -EIO;

  if ((ecx & 0x0100) != 0x0100)
    return -EIO;

  printf("%X %X %X\n", esi, edi, ecx);

  /* Now set. */
  asm volatile ("out %%al, %1\n\t"
		"out %%al, $0x4F"
		: "=a" (eax)
		: "d" (smapi_port1),
		  "a" (0x5380), "b" (start ? 0x2117 : 0x211b),
		  "c" (((bat+1) << 8) | val), "S" (esi), "D" (edi)
		: "cc");
  
  usleep(50000);

  errcode = (eax >> 8) & 0xFF;
  if (errcode == 0xA6)
    return -EAGAIN;
  else if (errcode != 0)
    return -EIO;

  return 0;
}

int main(int argc, char **argv)
{
  uint8_t start, stop;
  int err;
  int ret;

  fprintf(stderr, "Request IO permissions.\n");

  if (ioperm(smapi_port1, smapi_port1, 1) != 0) {
    perror("ioperm");
    return 1;
  }

  if (ioperm(smapi_port2, smapi_port2, 1) != 0) {
    perror("ioperm");
    return 1;
  }

  if (argc == 1) {
    ret = 0;
    fprintf(stderr, "Get BAT0 start.\n");
    err = get_threshold(0, 1, &start);
    if (err != 0) {
      errno = -err;
      perror("get_threshold");
      ret = 1;
      start = -1;
    }

    fprintf(stderr, "Get BAT0 stop.\n");
    err = get_threshold(0, 0, &stop);
    if (err != 0) {
      errno = -err;
      perror("get_threshold");
      ret = 1;
      stop = -1;
    }

    printf("start = %d, stop = %d\n", (int)start, (int)stop);
    return ret;
  } else if (argc == 3) {
    ret = 0;
    start = (uint8_t)atoi(argv[1]);
    stop = (uint8_t)atoi(argv[2]);

    fprintf(stderr, "Set BAT0 start.\n");
    err = set_threshold(0, 1, start);
    if (err != 0) {
      errno = -err;
      perror("set_threshold");
      ret = 1;
    }

    fprintf(stderr, "Set BAT0 stop.\n");
    err = set_threshold(0, 0, stop);
    if (err != 0) {
      errno = -err;
      perror("set_threshold");
      ret = 1;
    }

    return ret;
  }

  return 0;
}
