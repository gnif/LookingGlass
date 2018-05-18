.code

memcpySSE proc
  ; dst = rcx
  ; src = rdx
  ; len = r8

  test r8 , r8
  jne  OK
  ret

  OK:
  ; void * end = dst + (length & ~0x100);
  ; end = r10
  mov r9 , r8
  and r9 , -0100h
  mov r10, rcx
  add r10, r9

  ; size_t rem = (length & 0xFF) >> 4);
  ; rem = r11
  mov  r11, r8
  and  r11, 0FFh
  shr  r11, 4

  sub rsp, 8 + 10*16 + 4*8
  movdqa oword ptr [rsp + 4*8 + 00 ], xmm6
  movdqa oword ptr [rsp + 4*8 + 16 ], xmm7
  movdqa oword ptr [rsp + 4*8 + 32 ], xmm8
  movdqa oword ptr [rsp + 4*8 + 48 ], xmm9
  movdqa oword ptr [rsp + 4*8 + 64 ], xmm10
  movdqa oword ptr [rsp + 4*8 + 80 ], xmm11
  movdqa oword ptr [rsp + 4*8 + 96 ], xmm12
  movdqa oword ptr [rsp + 4*8 + 112], xmm13
  movdqa oword ptr [rsp + 4*8 + 128], xmm14
  movdqa oword ptr [rsp + 4*8 + 144], xmm15

  cmp rcx, r10
  je  RemainingBlocks

  FullLoop:
  vmovaps xmm0 , xmmword ptr [rdx + 000h]
  vmovaps xmm1 , xmmword ptr [rdx + 010h]
  vmovaps xmm2 , xmmword ptr [rdx + 020h]
  vmovaps xmm3 , xmmword ptr [rdx + 030h]
  vmovaps xmm4 , xmmword ptr [rdx + 040h]
  vmovaps xmm5 , xmmword ptr [rdx + 050h]
  vmovaps xmm6 , xmmword ptr [rdx + 060h]
  vmovaps xmm7 , xmmword ptr [rdx + 070h]
  vmovaps xmm8 , xmmword ptr [rdx + 080h]
  vmovaps xmm9 , xmmword ptr [rdx + 090h]
  vmovaps xmm10, xmmword ptr [rdx + 0A0h]
  vmovaps xmm11, xmmword ptr [rdx + 0B0h]
  vmovaps xmm12, xmmword ptr [rdx + 0C0h]
  vmovaps xmm13, xmmword ptr [rdx + 0D0h]
  vmovaps xmm14, xmmword ptr [rdx + 0E0h]
  vmovaps xmm15, xmmword ptr [rdx + 0F0h]
  vmovntdq xmmword ptr [rcx + 000h], xmm0
  vmovntdq xmmword ptr [rcx + 010h], xmm1
  vmovntdq xmmword ptr [rcx + 020h], xmm2
  vmovntdq xmmword ptr [rcx + 030h], xmm3
  vmovntdq xmmword ptr [rcx + 040h], xmm4
  vmovntdq xmmword ptr [rcx + 050h], xmm5
  vmovntdq xmmword ptr [rcx + 060h], xmm6
  vmovntdq xmmword ptr [rcx + 070h], xmm7
  vmovntdq xmmword ptr [rcx + 080h], xmm8
  vmovntdq xmmword ptr [rcx + 090h], xmm9
  vmovntdq xmmword ptr [rcx + 0A0h], xmm10
  vmovntdq xmmword ptr [rcx + 0B0h], xmm11
  vmovntdq xmmword ptr [rcx + 0C0h], xmm12
  vmovntdq xmmword ptr [rcx + 0D0h], xmm13
  vmovntdq xmmword ptr [rcx + 0E0h], xmm14
  vmovntdq xmmword ptr [rcx + 0F0h], xmm15
  add rdx, 0100h
  add rcx, 0100h
  cmp rcx, r10
  jne FullLoop

  RemainingBlocks:
  lea  r9 , JumpTable
  mov  r10, 15
  sub  r10, r11
  imul r10, 5
  add  r9 , r10
  jmp  r9

  JumpTable:
  jmp Block15
  jmp Block14
  jmp Block13
  jmp Block12
  jmp Block11
  jmp Block10
  jmp Block9
  jmp Block8
  jmp Block7
  jmp Block6
  jmp Block5
  jmp Block4
  jmp Block3
  jmp Block2
  jmp Block1
  jmp Block0

  ; ensure we generate near jumps
  padding1 db 127 dup(090h)

  Block15:
  vmovaps  xmm14, xmmword ptr [rdx + 0E0h]
  vmovntdq xmmword ptr [rcx + 0E0h], xmm14
  Block14:
  vmovaps  xmm13, xmmword ptr [rdx + 0D0h]
  vmovntdq xmmword ptr [rcx + 0D0h], xmm13
  Block13:
  vmovaps  xmm12, xmmword ptr [rdx + 0C0h]
  vmovntdq xmmword ptr [rcx + 0C0h], xmm12
  Block12:
  vmovaps  xmm11, xmmword ptr [rdx + 0B0h]
  vmovntdq xmmword ptr [rcx + 0B0h], xmm11
  Block11:
  vmovaps  xmm10, xmmword ptr [rdx + 0A0h]
  vmovntdq xmmword ptr [rcx + 0A0h], xmm10
  Block10:
  vmovaps  xmm9 , xmmword ptr [rdx + 090h]
  vmovntdq xmmword ptr [rcx + 090h], xmm9
  Block9:
  vmovaps  xmm8 , xmmword ptr [rdx + 080h]
  vmovntdq xmmword ptr [rcx + 080h], xmm8
  Block8:
  vmovaps  xmm7 , xmmword ptr [rdx + 070h]
  vmovntdq xmmword ptr [rcx + 070h], xmm7
  Block7:
  vmovaps  xmm6 , xmmword ptr [rdx + 060h]
  vmovntdq xmmword ptr [rcx + 060h], xmm6
  Block6:
  vmovaps  xmm5 , xmmword ptr [rdx + 050h]
  vmovntdq xmmword ptr [rcx + 050h], xmm5
  Block5:
  vmovaps  xmm4 , xmmword ptr [rdx + 040h]
  vmovntdq xmmword ptr [rcx + 040h], xmm4
  Block4:
  vmovaps  xmm3 , xmmword ptr [rdx + 030h]
  vmovntdq xmmword ptr [rcx + 030h], xmm3
  Block3:
  vmovaps  xmm2 , xmmword ptr [rdx + 020h]
  vmovntdq xmmword ptr [rcx + 020h], xmm2
  Block2:
  vmovaps  xmm1 , xmmword ptr [rdx + 010h]
  vmovntdq xmmword ptr [rcx + 010h], xmm1
  Block1:
  vmovaps  xmm0 , xmmword ptr [rdx + 000h]
  vmovntdq xmmword ptr [rcx + 000h], xmm0

  imul r11, 16
  add  rdx, r11
  add  rcx, r11

  Block0:
  movdqa xmm6 , oword ptr [rsp + 4*8 + 00 ]
  movdqa xmm7 , oword ptr [rsp + 4*8 + 16 ]
  movdqa xmm8 , oword ptr [rsp + 4*8 + 32 ]
  movdqa xmm9 , oword ptr [rsp + 4*8 + 48 ]
  movdqa xmm10, oword ptr [rsp + 4*8 + 64 ]
  movdqa xmm11, oword ptr [rsp + 4*8 + 80 ]
  movdqa xmm12, oword ptr [rsp + 4*8 + 96 ]
  movdqa xmm13, oword ptr [rsp + 4*8 + 112]
  movdqa xmm14, oword ptr [rsp + 4*8 + 128]
  movdqa xmm15, oword ptr [rsp + 4*8 + 144]
  add rsp, 8 + 10*16 + 4*8

  and  r8, 0Fh
  imul r8, 5
  lea  r9, CopyTable
  add  r9, r8
  jmp  r9

  CopyTable:
  ret
  nop
  nop
  nop
  nop

  jmp Copy1
  jmp Copy2
  jmp Copy3
  jmp Copy4
  jmp Copy5
  jmp Copy6
  jmp Copy7
  jmp Copy8
  jmp Copy9
  jmp Copy10
  jmp Copy11
  jmp Copy12
  jmp Copy13
  jmp Copy14

  ; copy 15
  mov r8  , qword ptr [rdx + 00h]
  mov r9d , dword ptr [rdx + 08h]
  mov r10w,  word ptr [rdx + 0Ch]
  mov al  ,  byte ptr [rdx + 0Eh]
  mov qword ptr [rcx + 00h], r8
  mov dword ptr [rcx + 08h], r9d
  mov  word ptr [rcx + 0Ch], r10w
  mov  byte ptr [rcx + 0Eh], al
  ret

  ; ensure we generate near jumps
  padding2 db 127 dup(090h)

  Copy1:
  mov al, byte ptr [rdx]
  mov byte ptr [rcx], al
  ret

  Copy2:
  mov r10w, word ptr [rdx]
  mov word ptr [rcx], r10w
  ret

  Copy3:
  mov r10w, word ptr [rdx]
  mov word ptr [rcx], r10w
  mov al, byte ptr [rdx + 02h]
  mov byte ptr [rcx + 02h], al
  ret

  Copy4:
  mov r9d , dword ptr [rdx]
  mov dword ptr [rcx], r9d
  ret

  Copy5:
  mov r9d , dword ptr [rdx]
  mov dword ptr [rcx], r9d
  mov al, byte ptr [rdx + 04h]
  mov byte ptr [rcx + 04h], al
  ret

  Copy6:
  mov r9d , dword ptr [rdx]
  mov dword ptr [rcx], r9d
  mov r10w, word ptr [rdx + 04h]
  mov word ptr [rcx + 04h], r10w
  ret

  Copy7:
  mov r9d , dword ptr [rdx]
  mov dword ptr [rcx], r9d
  mov r10w, word ptr [rdx + 04h]
  mov word ptr [rcx + 04h], r10w
  mov al, byte ptr [rdx + 06h]
  mov byte ptr [rcx + 06h], al
  ret

  Copy8:
  mov r8, qword ptr [rdx]
  mov qword ptr [rcx], r8
  ret

  Copy9:
  mov r8, qword ptr [rdx]
  mov qword ptr [rcx], r8
  mov al, byte ptr [rdx + 08h]
  mov byte ptr [rcx + 08h], al
  ret

  Copy10:
  mov r8, qword ptr [rdx]
  mov qword ptr [rcx], r8
  mov r10w, word ptr [rdx + 08h]
  mov word ptr [rcx + 08h], r10w
  ret

  Copy11:
  mov r8, qword ptr [rdx]
  mov qword ptr [rcx], r8
  mov r10w, word ptr [rdx + 08h]
  mov word ptr [rcx + 08h], r10w
  mov al, byte ptr [rdx + 0Ah]
  mov byte ptr [rcx + 0Ah], al
  ret

  Copy12:
  mov r8, qword ptr [rdx]
  mov qword ptr [rcx], r8
  mov r9d , dword ptr [rdx + 08h]
  mov dword ptr [rcx + 08h], r9d
  ret

  Copy13:
  mov r8, qword ptr [rdx]
  mov qword ptr [rcx], r8
  mov r9d , dword ptr [rdx + 08h]
  mov dword ptr [rcx + 08h], r9d
  mov al, byte ptr [rdx + 0Ch]
  mov byte ptr [rcx + 0Ch], al
  ret

  Copy14:
  mov r8  , qword ptr [rdx      ]
  mov r9d , dword ptr [rdx + 08h]
  mov r10w,  word ptr [rdx + 0Ch]
  mov qword ptr [rcx      ], r8
  mov dword ptr [rcx + 08h], r9d
  mov  word ptr [rcx + 0Ch], r10w
  ret

memcpySSE endp
end