/* -----------------------------------------------------------------------
   ffi.c - Copyright (c) 2012 ZAO "MCST".
   
   E2K Foreign Function Interface 

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   ``Software''), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED ``AS IS'', WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
   ----------------------------------------------------------------------- */

#if 0
#define DEBUG
#endif

#include <ffi.h>
#include <ffi_common.h>

#include <stdlib.h>

#ifdef DEBUG
#include <stdio.h>
#endif

/* Minimal stack alignment */
#define STACK_ALIGN 16

/* Parameter slot size */
#define SLOT_SIZE 8

/* Максимальное количество слотов, используемых для передачи через регистры */
#define MAX_ARG_REG_SLOTS 8

/* Максимальный размер результата, передаваемый на регистрах */
#define MAX_RES_REG_SIZE 64

#define MIN(x,y) (((x) < (y)) ? (x) : (y))
#define MAX(x,y) (((x) > (y)) ? (x) : (y))

/* ------------------------------------------------------------------------- */

static unsigned calc_arg_alignment(const ffi_type *arg_type)
{
  if (arg_type->size <= SLOT_SIZE)
    return SLOT_SIZE;
  else
    return SLOT_SIZE * 2;
}

/* Вычисление количество стекового пространства, которое требуется
 * зарезервироать для передачи параметров */
static unsigned calc_args_stack_size(const ffi_cif *cif)
{
  unsigned n, pos;

  for (n = 0, pos = 0; n < cif->nargs; n++)
    {
      ffi_type *arg_type = cif->arg_types[n];

      pos = FFI_ALIGN(pos, calc_arg_alignment(arg_type));
      pos += arg_type->size;
    }
  pos = FFI_ALIGN(pos, STACK_ALIGN);

  return pos;
}

/* Вычисление количество регистров, которое потребуется при передаче
 * параметров */
unsigned
calc_args_reg_size(const ffi_cif *cif)
{
  unsigned n, pos, nregs;

  nregs = 0;
  for (n = 0, pos = 0; n < cif->nargs; n++)
    {
      ffi_type *arg_type = cif->arg_types[n];

      /* По аналогии с calc_args_stack_size сначала вычислим местоположение
       * дальнего конца аргумента в стеке */
      pos = FFI_ALIGN(pos, calc_arg_alignment(arg_type));
      pos += arg_type->size;

      /* Если агумент целиком влезает в регистровую область, значит
       * он будет передан на регистрах. В противном случае будет передан
       * в стеке (и часть регистров останется неиспользованной) */
      if (pos <= (SLOT_SIZE * MAX_ARG_REG_SLOTS))
        nregs = FFI_ALIGN(pos, SLOT_SIZE) / SLOT_SIZE;
      else
        break;
    }

  return nregs;
}

static unsigned calc_result_size(const ffi_type *type)
{
  /* man: rvalue must point to storage that is sizeof(ffi_arg) or larger
   * for non-floating point types */
  switch (type->type)
    {
    case FFI_TYPE_INT:
    case FFI_TYPE_UINT8:
    case FFI_TYPE_SINT8:
    case FFI_TYPE_UINT16:
    case FFI_TYPE_SINT16:
    case FFI_TYPE_UINT32:
    case FFI_TYPE_SINT32:
      return MAX(type->size, sizeof(ffi_arg));
    default:
      return type->size;
    }
}

static unsigned get_type_real_size(const ffi_type *type)
{
  if (type->type == FFI_TYPE_VOID)
    return 0;
  else
    return type->size;
}

unsigned
calc_aligned_stack_result_size (const ffi_cif *cif)
{
  return FFI_ALIGN (get_type_real_size (cif->rtype), STACK_ALIGN);
}

/* ------------------------------------------------------------------------- */

/* Perform machine dependent cif processing */
ffi_status ffi_prep_cif_machdep(ffi_cif *cif)
{
  /* У людей принято размер стека под аргументы считать именно здесь */
  cif->bytes = calc_args_stack_size(cif);
  return FFI_OK;
}

extern void ffi_call_asm (void (*fn)(void), void *stack_image,
                          unsigned total_size,
                          unsigned params_size, unsigned result_size,
			  void *closure);

static void
ffi_call_int (ffi_cif *cif, void (*fn) (void), void *rvalue, void **avalue,
	      void *closure)
{
  unsigned n, pos;
  char *stack_image;

  /* Size of the arguments */
  unsigned const params_size = cif->bytes;

  /* Size of the result */
  unsigned const result_size = calc_result_size(cif->rtype);

  /* Total size of the stack frame. The arguments and the result are passed
   * in the same stack area (outgoing parameters stack) */
  unsigned const total_size = FFI_ALIGN(MAX(params_size, result_size), STACK_ALIGN);

#ifdef DEBUG
  printf("ffi_call: result_size=%d, params_size=%d, total_size=%d\n",
         result_size, params_size, total_size);
#endif

  /* Allocate memory for stack image of parameters and results */
  stack_image = alloca(total_size);
  for (n = 0; n < total_size; n++)
    stack_image[n] = 0;

  /* Copy parameter values into stack image */
  for (n = 0, pos = 0; n < cif->nargs; n++)
    {
      ffi_type *arg_type = cif->arg_types[n];
      char *arg_image;

#ifdef DEBUG
#if 0
      printf("ffi_call: param %d: type=%d, size=%d\n",
             n, arg_type->type, (int)arg_type->size);
#endif
#endif

      /* Вычисляем стартовую позицию аргумента в стеке */
      pos = FFI_ALIGN(pos, calc_arg_alignment(arg_type));

      /* Копируем байтовый образ аргумента в образ стека */
      arg_image = stack_image + pos;
      memcpy(arg_image, avalue[n], arg_type->size);

      /* Позиция "после аргумента" в стеке (то, что переходит на следующую
       * итерацию цикла) */
      pos += arg_type->size;

      /* do promotion */
      switch (arg_type->type)
        {
        case FFI_TYPE_SINT8:
          *((long long*) arg_image) = *((SINT8*)arg_image);
          break;
        case FFI_TYPE_UINT8:
          *((unsigned long long*) arg_image) = *((UINT8*)arg_image);
          break;
        case FFI_TYPE_SINT16:
          *((long long*) arg_image) = *((SINT16*)arg_image);
          break;
        case FFI_TYPE_UINT16:
          *((unsigned long long*) arg_image) = *((UINT16*)arg_image);
          break;
        case FFI_TYPE_SINT32:
          *((long long*) arg_image) = *((SINT32*)arg_image);
          break;
        case FFI_TYPE_UINT32:
          *((unsigned long long*) arg_image) = *((UINT32*)arg_image);
          break;
        }
    }
  pos = FFI_ALIGN(pos, STACK_ALIGN);
  FFI_ASSERT(pos == params_size);

#ifdef DEBUG
#if 0
  for (n = 0; n < params_size; n++)
    {
      printf("%02x ", (unsigned char)stack_image[n]);
      if ((n % 8) == 7)
        printf("\n");
    }
  printf("\n");
#endif
#endif

  /* Do call */
  ffi_call_asm (fn, stack_image, total_size, params_size, result_size,
		closure);

  /* Copy the result value from stack image */
  if (rvalue != NULL)
    memcpy(rvalue, stack_image, result_size);
}

void
ffi_call (ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue)
{
  ffi_call_int (cif, fn, rvalue, avalue, NULL);
}

/* ------------------------------------------------------------------------- */

extern void ffi_closure_e2k (void) FFI_HIDDEN;
extern void ffi_go_closure_e2k (void) FFI_HIDDEN;

ffi_status
ffi_prep_closure_loc (ffi_closure *closure,
		      ffi_cif *cif,
		      void (*fun)(ffi_cif*, void*, void**, void*),
		      void *user_data,
		      void *codeloc)
{
  /* Местоположение трамплина */
  unsigned int *tramp = (unsigned int *) &closure->tramp[0];

  /* Функция, на которую выполняется переход из трамплина */
  unsigned long fn = (unsigned long) ffi_closure_e2k;

  /* Значение closure, которое будет передано в fn */
  unsigned long clx = (unsigned long) closure;

  /* Если размер результата больше 64 байт, то память под него выделена
   * вызывающей процедурой. Но эта память пересекается с памятью под
   * параметры. Интерфейс пользовательской функции, вызываемой из-под
   * closure, позволяет пользователю формировать результат раньше, чем
   * будут прочтены параметры. Поэтому в этом случае требуется промежуточный
   * буфер под результат. А после вызова пользовательской функции
   * нужно будет скопировать результат из буфера в стек вызвавшей
   * процедуры
   *
   * Если размер результата меньше или развен 64 байтам, то памать под
   * него вообще никто не выделяет. А потому в этом случае так же понадобится
   * промежуточный буфер. Но, в отличие от предыдущего случая, потом из этого
   * бефера надо будет скопировать на регистры
   *
   * В любом случае буфер через alloca можно выделить только внутри
   * ffi_closure_e2k, т.к. именно там бедет копирование из буфера
   * на регистры (потому что больше нигде эти регистры доступны не будут).
   * Размер нужно выравнивать под выровненность стека */

  if (cif->abi != FFI_E2K)
    return FFI_BAD_ABI;

#ifdef DEBUG
  printf ("ffi_prep_closure_loc: res_buffer_size = %d\n", res_buffer_size);
  printf ("ffi_prep_closure_loc: incoming_regs = %d\n", incoming_regs);
  printf ("ffi_prep_closure_loc: fn=%lx\n", fn);
  printf ("ffi_prep_closure_loc: clx=%lx\n", clx);
#endif

  /* Согласовано с FFI_TRAMPOLINE_SIZE */
#ifdef __ptr64__
  tramp[0]  = 0x0c000032;            /* HS    0c000032 : */
  tramp[1]  = 0x61c0dcd1;            /* ALS0  61c0dcd1  movtd,0 _f64,_lts0 <ffi_closure_e2k>, %ctpr1    */
  tramp[2]  = 0x11c0deec;            /* ALS1  11c0deec  addd,1 0x0, _f64,_lts2 <closure>, %dg12         */
  tramp[3]  = 0x00000000;            /*       00000000                                                  */
  tramp[4]  = (unsigned)(clx >> 32); /* LTS3  xxxxxxxx  <closure>.hi                                    */
  tramp[5] =  (unsigned)(clx);       /* LTS2  xxxxxxxx  <closure>.lo                                    */
  tramp[6]  = (unsigned)(fn >> 32);  /* LTS1  xxxxxxxx  <ffi_closure_e2k.hi>                            */
  tramp[7]  = (unsigned)(fn);        /* LTS0  xxxxxxxx  <ffi_closure_e2k.lo>                            */
  tramp[8] =  0x00001001;            /* HS    00001001 :                                                */
  tramp[9] =  0x80000420;            /* SS    80000420  ct %ctpr1                                       */
#else
  tramp[0]  = 0x0c000022;            /* HS    0c000022 : */
  tramp[1]  = 0x63f0d8d1;            /* ALS0  63f0d8d1  getpl,0 _f32s,_lts0 <ffi_closure_e2k>, %ctpr1   */
  tramp[2]  = 0x11c0d9ec;            /* ALS1  11c0d9ec  addd,1 0x0, _f32s,_lts1 <closure>, %dg12        */
  tramp[3]  = 0x00000000;            /* LTS2  00000000                                                  */
  tramp[4]  = (unsigned)(clx);       /* LTS1  xxxxxxxx  <closure>                                       */
  tramp[5]  = (unsigned)(fn);        /* LTS0  xxxxxxxx  <ffi_closure_e2k>                               */
  tramp[6] =  0x00001001;            /* HS    00001001 :                                                */
  tramp[7] =  0x80000420;            /* SS    80000420  ct %ctpr1                                       */
#endif

  closure->cif = cif;
  closure->fun = fun;
  closure->user_data = user_data;

  __builtin___clear_cache ((char *) tramp,
			   (char *) tramp + FFI_TRAMPOLINE_SIZE);

  return FFI_OK;
}

unsigned
ffi_closure_e2k_inner (ffi_cif *cif,
		       void (*fun)(ffi_cif*, void*, void**, void*),
		       void *user_data,
                       void *rvalue,
		       unsigned long stack_params)
{
  void **avalue = alloca(cif->nargs * sizeof(void *));
  unsigned n, pos;

  /* Grab the addresses of the arguments from the stack frame.  */
  for (n = 0, pos = 0; n < cif->nargs; n++)
    {
      ffi_type *arg_type = cif->arg_types[n];

      pos = FFI_ALIGN(pos, calc_arg_alignment(arg_type));
      avalue[n] = (void*)(stack_params + pos);
      pos += arg_type->size;
    }

#ifdef DEBUG
#if 0
  {
    unsigned long long *p = (unsigned long long*)stack_params;
    printf("rvalue = %p\n", rvalue);
    printf("%p: %016llx\n", p, p[0]);
    printf("%p: %016llx\n", p+1, p[1]);
    printf("%p: %016llx\n", p+2, p[2]);
    printf("%p: %016llx\n", p+3, p[3]);
    printf("%p: %016llx\n", p+4, p[4]);
    printf("%p: %016llx\n", p+5, p[5]);
  }
#endif
#endif

  /* Invoke the closure.  */
  fun (cif, rvalue, avalue, user_data);

  return get_type_real_size(cif->rtype);
}

/* ------------------------------------------------------------------------- */

void
ffi_call_go (ffi_cif *cif, void (*fn)(void), void *rvalue,
	     void **avalue, void *closure)
{
  ffi_call_int (cif, fn, rvalue, avalue, closure);
}

ffi_status
ffi_prep_go_closure (ffi_go_closure *closure, ffi_cif *cif,
		     void (*fun)(ffi_cif*, void*, void**, void*))
{
  if (cif->abi != FFI_E2K)
    return FFI_BAD_ABI;

  closure->tramp = ffi_go_closure_e2k;
  closure->cif = cif;
  closure->fun = fun;

  return FFI_OK;
}
