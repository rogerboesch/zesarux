/*
    ZEsarUX  ZX Second-Emulator And Released for UniX
    Copyright (C) 2013 Cesar Hernandez Bano

    This file is part of ZEsarUX.

    ZEsarUX is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <sys/time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cpu.h"
#include "debug.h"
#include "operaciones.h"
#include "zx8081.h"
#include "mem128.h"
#include "ay38912.h"
#include "compileoptions.h"
#include "tape_smp.h"
#include "audio.h"
#include "screen.h"
#include "menu.h"
#include "tape.h"
#include "snap.h"
#include "snap_z81.h"
#include "snap_zx8081.h"
#include "utils.h"
#include "ula.h"
#include "joystick.h"
#include "realjoystick.h"
#include "z88.h"
#include "chardetect.h"
#include "jupiterace.h"
#include "cpc.h"
#include "timex.h"
#include "zxuno.h"
#include "ulaplus.h"
#include "chloe.h"
#include "prism.h"
#include "diviface.h"
#include "snap_rzx.h"


#include "autoselectoptions.h"

#if defined(__APPLE__)
        #include <sys/syslimits.h>
#endif

#define ZSF_NOOP_ID 0
#define ZSF_MACHINEID 1
#define ZSF_Z80_REGS_ID 2
#define ZSF_MOTO_REGS_ID 3
#define ZSF_RAMBLOCK 4


int zsf_force_uncompressed=0; //Si forzar bloques no comprimidos

/*
Format ZSF:
* All numbers are LSB

Every block is defined with a header:

2 bytes - 16 bit: block ID
4 bytes - 32 bit: block Lenght
After these 6 bytes, the data for the block comes.



-Block ID 0: ZSF_NOOP
No operation. Block lenght 0

-Block ID 1: ZSF_MACHINEID
Defines which machine is this snapshot. Normally it should come after any other block, but can appear later
Even it could be absent, so the snapshot will be loaded according to the current machine

Byte fields:
0: Machine ID. Same ID  defined on function set_machine_params from cpu.c

-Block ID 2: ZSF_Z80_REGS
Z80 CPU Registers

-Block ID 3: ZSF_MOTO_REGS
Motorola CPU Registers

-Block ID 4: ZSF_RAMBLOCK
A ram binary block
Byte Fields:
0: Flags. Currently: bit 0: if compressed with repetition block DD DD YY ZZ, where 
    YY is the byte to repeat and ZZ the number of repetitions (0 means 256)
1,2: Block start address
3,4: Block lenght
5 and next bytes: data bytes


-Como codificar bloques de memoria para Spectrum 128k, zxuno, tbblue, tsconf, etc?
Con un numero de bloque (0...255) pero... que tamaño de bloque? tbblue usa paginas de 8kb, tsconf usa paginas de 16kb
Quizá numero de bloque y parametro que diga tamaño, para tener un block id comun para todos ellos

*/

//Maxima longitud de los bloques de descripcion
#define MAX_ZSF_BLOCK_ID_NAMELENGTH 30

//Total de nombres sin contar el unknown final
#define MAX_ZSF_BLOCK_ID_NAMES 5
char *zsf_block_id_names[]={
 //123456789012345678901234567890
  "ZSF_NOOP",
  "ZSF_MACHINEID",
  "ZSF_Z80_REGS",
  "ZSF_MOTO_REGS",
  "ZSF_RAMBLOCK",

  "Unknown"  //Este siempre al final
};

char *zsf_get_block_id_name(int block_id)
{
  if (block_id>=MAX_ZSF_BLOCK_ID_NAMES) return zsf_block_id_names[MAX_ZSF_BLOCK_ID_NAMES];
  else return zsf_block_id_names[block_id];
}

int zsf_write_block(FILE *ptr_zsf_file, z80_byte *source,z80_int block_id, unsigned int lenght)
{
  z80_byte block_header[6];
  block_header[0]=value_16_to_8l(block_id);
  block_header[1]=value_16_to_8h(block_id);

  block_header[2]=(lenght)      & 0xFF;
  block_header[3]=(lenght>>8)   & 0xFF;
  block_header[4]=(lenght>>16)  & 0xFF;
  block_header[5]=(lenght>>24)  & 0xFF;

  //Write header
  fwrite(block_header, 1, 6, ptr_zsf_file);

  //Write data block
  if (lenght) fwrite(source, 1, lenght, ptr_zsf_file);

  return 0;

}

void load_zsf_snapshot_z80_regs(z80_byte *header)
{
  reg_c=header[0];
  reg_b=header[1];
  reg_e=header[2];
  reg_d=header[3];
  reg_l=header[4];
  reg_h=header[5];

        store_flags(header[6]);
        reg_a=header[7];

        reg_ix=value_8_to_16(header[9],header[8]);
        reg_iy=value_8_to_16(header[11],header[10]);

        reg_c_shadow=header[12];
        reg_b_shadow=header[13];
        reg_e_shadow=header[14];
        reg_d_shadow=header[15];
        reg_l_shadow=header[16];
        reg_h_shadow=header[17];

        store_flags_shadow(header[18]);
        reg_a_shadow=header[19];

        reg_r=header[20];
        reg_r_bit7=reg_r&128;

        reg_i=header[21];

        reg_sp=value_8_to_16(header[23],header[22]);

        reg_pc=value_8_to_16(header[25],header[24]);

        im_mode=header[26] & 2;
        if (im_mode==1) im_mode=2;

        iff1.v=iff2.v=header[26] &1;
}


/*
Cargar bloque de datos en destino indicado
block_length: usado en bloques no comprimidos (lo que dice la cabecera que ocupa)
longitud_original: usado en bloques comprimidos (lo que ocupan los bloques comprimidos)
*/
void load_zsf_snapshot_block_data_addr(z80_byte *block_data,z80_byte *destino,int block_lenght, int longitud_original,int si_comprimido)
{
  
  //printf ("load_zsf_snapshot_block_data_addr block_lenght: %d longitud_original: %d si_comprimido: %d\n",block_lenght,longitud_original,si_comprimido);

  if (si_comprimido) {
    //Comprimido
    util_uncompress_data_repetitions(block_data,destino,longitud_original,0xDD);
  }


  else {
    int i=0;
    while (block_lenght) {
      //poke_byte_no_time(block_start++,block_data[i++]);
      *destino=block_data[i++];
      destino++;
      block_lenght--;
    }
  }
}

void load_zsf_snapshot_block_data(z80_byte *block_data,int longitud_original)
{



  int i=0;
  z80_byte block_flags=block_data[i];

  //longitud_original : tamanyo que ocupa todo el bloque con la cabecera de 5 bytes

  i++;
  z80_int block_start=value_8_to_16(block_data[i+1],block_data[i]);
  i +=2;
  z80_int block_lenght=value_8_to_16(block_data[i+1],block_data[i]);
  i+=2;

  debug_printf (VERBOSE_DEBUG,"Block start: %d Lenght: %d Compressed: %d Length_source: %d",block_start,block_lenght,block_flags&1,longitud_original);
  //printf ("Block start: %d Lenght: %d Compressed: %d Length_source: %d\n",block_start,block_lenght,block_flags&1,longitud_original);


  longitud_original -=5;


  load_zsf_snapshot_block_data_addr(&block_data[i],&memoria_spectrum[block_start],block_lenght,longitud_original,block_flags&1);

  /*if (block_flags&1) {
    //Comprimido
    util_uncompress_data_repetitions(&block_data[i],&memoria_spectrum[block_start],longitud_original,0xDD);
    return;
  }


  else {
    while (block_lenght) {
      poke_byte_no_time(block_start++,block_data[i++]);
      block_lenght--;
    }
  }
  */
}


void load_zsf_snapshot(char *filename)
{

  FILE *ptr_zsf_file;

  ptr_zsf_file=fopen(filename,"rb");
  if (!ptr_zsf_file) {
          debug_printf (VERBOSE_ERR,"Error reading snapshot file %s",filename);
          return;
  }

  z80_byte block_header[6];

  //Read blocks
  while (!feof(ptr_zsf_file)) {
    //Read header block
    unsigned int leidos=fread(block_header,1,6,ptr_zsf_file);
    if (leidos==0) break; //End while

    if (leidos!=6) {
      debug_printf(VERBOSE_ERR,"Error reading snapshot file. Read: %u Expected: 6",leidos);
      return;
    }

    z80_int block_id;
    block_id=value_8_to_16(block_header[1],block_header[0]);
    unsigned int block_lenght=block_header[2]+(block_header[3]*256)+(block_header[4]*65536)+(block_header[5]*16777216);

    debug_printf (VERBOSE_INFO,"Block id: %u (%s) Lenght: %u",block_id,zsf_get_block_id_name(block_id),block_lenght);

    z80_byte *block_data;

    //Evitar bloques de longitud cero
    //Por si acaso inicializar a algo
    z80_byte buffer_nothing;
    block_data=&buffer_nothing;

    if (block_lenght) {
      block_data=malloc(block_lenght);

      if (block_data==NULL) {
        debug_printf(VERBOSE_ERR,"Error allocation memory reading ZSF file");
        return;
      }

      //Read block data
      leidos=fread(block_data,1,block_lenght,ptr_zsf_file);
      if (leidos!=block_lenght) {
        debug_printf(VERBOSE_ERR,"Error reading snapshot file. Read: %u Expected: %u",leidos,block_lenght);
        return;
      }
    }

    //switch for every possible block id
    switch(block_id)
    {

      case ZSF_NOOP_ID:
        //no hacer nada
      break;

      case ZSF_MACHINEID:
        current_machine_type=*block_data;
        set_machine(NULL);
        reset_cpu();
      break;

      case ZSF_Z80_REGS_ID:
        load_zsf_snapshot_z80_regs(block_data);
      break;

      case ZSF_RAMBLOCK:
        load_zsf_snapshot_block_data(block_data,block_lenght);
      break;

      default:
        debug_printf(VERBOSE_ERR,"Unknown ZSF Block ID: %u. Continue anyway",block_id);
      break;

    }


    if (block_lenght) free(block_data);

  }

  fclose(ptr_zsf_file);


}

void save_zsf_snapshot_cpuregs(FILE *ptr)
{

  z80_byte header[27];

  header[0]=reg_c;
  header[1]=reg_b;
  header[2]=reg_e;
  header[3]=reg_d;
  header[4]=reg_l;
  header[5]=reg_h;

  header[6]=get_flags();
  header[7]=reg_a;



  header[8]=value_16_to_8l(reg_ix);
  header[9]=value_16_to_8h(reg_ix);
  header[10]=value_16_to_8l(reg_iy);
  header[11]=value_16_to_8h(reg_iy);

  header[12]=reg_c_shadow;
  header[13]=reg_b_shadow;
  header[14]=reg_e_shadow;
  header[15]=reg_d_shadow;
  header[16]=reg_l_shadow;
  header[17]=reg_h_shadow;



  header[18]=get_flags_shadow();
  header[19]=reg_a_shadow;

  header[20]=(reg_r&127) | (reg_r_bit7&128);

  header[21]=reg_i;

  header[22]=value_16_to_8l(reg_sp);
  header[23]=value_16_to_8h(reg_sp);
  header[24]=value_16_to_8l(reg_pc);
  header[25]=value_16_to_8h(reg_pc);


  z80_byte bits_estado=(iff1.v) | (im_mode==2 ? 2 : 0);
  header[26]=bits_estado;

  zsf_write_block(ptr, header,ZSF_Z80_REGS_ID, 27);

}

//Guarda en destino el bloque de memoria comprimido, siempre que salga a cuenta comprimirlo. 
//Si ocupa mas que el original, lo guardara comprimido
//Retorna longitud en valor retorno, flag indicando si esta comprimido o no
//Tener en cuenta que el destino sea al menos de tamanyo el doble que origen
int save_zsf_copyblock_compress_uncompres(z80_byte *origen,z80_byte *destino,int tamanyo_orig,int *si_comprimido)
{

  int longitud_comprimido=util_compress_data_repetitions(origen,destino,tamanyo_orig,0xDD);

  if (zsf_force_uncompressed || longitud_comprimido>tamanyo_orig) {
    *si_comprimido=0;
    memcpy(destino,origen,tamanyo_orig);
    return tamanyo_orig;
  }

  else {
    *si_comprimido=1;
    return longitud_comprimido;
  }
}

void save_zsf_snapshot(char *filename)
{

  FILE *ptr_zsf_file;

  //Save header File
  ptr_zsf_file=fopen(filename,"wb");
  if (!ptr_zsf_file) {
          debug_printf (VERBOSE_ERR,"Error writing snapshot file %s",filename);
          return;
  }

  //First save machine ID
  z80_byte save_machine_id=current_machine_type;
  zsf_write_block(ptr_zsf_file, &save_machine_id,ZSF_MACHINEID, 1);



  //Save cpu registers. Z80 or Moto or MK14
  if (CPU_IS_MOTOROLA) {

  }
  else if (CPU_IS_SCMP) {
  }

  else {
    save_zsf_snapshot_cpuregs(ptr_zsf_file);
  }

    //TODO: estado ula: puerto 254, etc


  //Maquinas de 48kb

  if (MACHINE_IS_SPECTRUM_16_48) {

	int longitud_ram=49152;
	if (MACHINE_IS_SPECTRUM_16) longitud_ram=16384;

  //Test. Save 48kb block
  //Allocate 5+48kb bytes
  z80_byte *ramblock=malloc(longitud_ram+5);
  if (ramblock==NULL) {
    debug_printf (VERBOSE_ERR,"Error allocating memory");
    return;
  }


  //Para el bloque comprimido
   z80_byte *compressed_ramblock=malloc(longitud_ram*2);
  if (ramblock==NULL) {
    debug_printf (VERBOSE_ERR,"Error allocating memory");
    return;
  }

  /*
  0: Flags. Currently: bit 0: if compressed
  1,2: Block start
  3,4: Block lenght
  5 and next bytes: data bytes
  */

  compressed_ramblock[0]=0;
  compressed_ramblock[1]=value_16_to_8l(16384);
  compressed_ramblock[2]=value_16_to_8h(16384);
  compressed_ramblock[3]=value_16_to_8l(longitud_ram);
  compressed_ramblock[4]=value_16_to_8h(longitud_ram);

  //Copy spectrum memory to ramblock
  int i;
  for (i=0;i<longitud_ram;i++) ramblock[5+i]=peek_byte_no_time(16384+i);

  int si_comprimido;
  int longitud_bloque=save_zsf_copyblock_compress_uncompres(&memoria_spectrum[16384],&compressed_ramblock[5],longitud_ram,&si_comprimido);
  if (si_comprimido) compressed_ramblock[0]|=1;

  //Store block to file
  zsf_write_block(ptr_zsf_file, compressed_ramblock,ZSF_RAMBLOCK, longitud_bloque+5);


  }


  free(ramblock);

  //test
  //save_zsf_snapshot_cpuregs(ptr_zsf_file);
  //test meter un NOOP
  zsf_write_block(ptr_zsf_file, NULL,0, 0);


  fclose(ptr_zsf_file);

}
