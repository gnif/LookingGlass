.code
memcpySSE proc
  ; dst = rcx
  ; src = rdx
  ; len = r8

  test r8, r8
  jz   @Exit
  cmp  rcx, rdx
  je   @Exit

  sub rsp, 8 + 2*16 + 4*8
  movdqa oword ptr [rsp + 4*8 + 00 ], xmm6
  movdqa oword ptr [rsp + 4*8 + 16 ], xmm7

  ; void * end = dst + (length & ~0x7F);
  ; end = r10
  mov r9 , r8
  and r9 , -07Fh
  jz  @RemainingBlocks
  mov r10, rcx
  add r10, r9

  @FullLoop:
  vmovaps xmm0 , xmmword ptr [rdx + 000h]
  vmovaps xmm1 , xmmword ptr [rdx + 010h]
  vmovaps xmm2 , xmmword ptr [rdx + 020h]
  vmovaps xmm3 , xmmword ptr [rdx + 030h]
  vmovaps xmm4 , xmmword ptr [rdx + 040h]
  vmovaps xmm5 , xmmword ptr [rdx + 050h]
  vmovaps xmm6 , xmmword ptr [rdx + 060h]
  vmovaps xmm7 , xmmword ptr [rdx + 070h]
  vmovntdq xmmword ptr [rcx + 000h], xmm0
  vmovntdq xmmword ptr [rcx + 010h], xmm1
  vmovntdq xmmword ptr [rcx + 020h], xmm2
  vmovntdq xmmword ptr [rcx + 030h], xmm3
  vmovntdq xmmword ptr [rcx + 040h], xmm4
  vmovntdq xmmword ptr [rcx + 050h], xmm5
  vmovntdq xmmword ptr [rcx + 060h], xmm6
  vmovntdq xmmword ptr [rcx + 070h], xmm7
  add rdx, 080h
  add rcx, 080h
  cmp rcx, r10
  jne @FullLoop

  @RemainingBlocks:
  ; size_t rem = (length & 0x7F) >> 4);
  ; rem = r11
  mov  r11, r8
  and  r11, 07Fh
  jz   @RestoreExit
  shr  r11, 4

  mov  r10, 7
  sub  r10, r11
  imul r10, 10
  lea  r9 , @FinalBlocks
  add  r9 , r10
  jmp  r9

  @RestoreExit:
  movdqa xmm6 , oword ptr [rsp + 4*8 + 00 ]
  movdqa xmm7 , oword ptr [rsp + 4*8 + 16 ]
  add rsp, 8 + 2*16 + 4*8

  @Exit:
  sfence
  ret

  @FinalBlocks:
  vmovaps  xmm6 , xmmword ptr [rdx + 060h]
  vmovntdq xmmword ptr [rcx + 060h], xmm6
  vmovaps  xmm5 , xmmword ptr [rdx + 050h]
  vmovntdq xmmword ptr [rcx + 050h], xmm5
  vmovaps  xmm4 , xmmword ptr [rdx + 040h]
  vmovntdq xmmword ptr [rcx + 040h], xmm4
  vmovaps  xmm3 , xmmword ptr [rdx + 030h]
  vmovntdq xmmword ptr [rcx + 030h], xmm3
  vmovaps  xmm2 , xmmword ptr [rdx + 020h]
  vmovntdq xmmword ptr [rcx + 020h], xmm2
  vmovaps  xmm1 , xmmword ptr [rdx + 010h]
  vmovntdq xmmword ptr [rcx + 010h], xmm1
  vmovaps  xmm0 , xmmword ptr [rdx + 000h]
  vmovntdq xmmword ptr [rcx + 000h], xmm0
  nop
  nop

  imul r9, 16
  add  rdx, r9
  add  rcx, r9

  @EndBlocks:
  and r8, 0Fh
  test r8, r8
  je  @RestoreExit

  cmp r8, 2
  je  @Copy2
  cmp r8, 3
  je  @Copy3
  cmp r8, 4
  je  @Copy4
  cmp r8, 5
  je  @Copy5
  cmp r8, 6
  je  @Copy6
  cmp r8, 7
  je  @Copy7
  cmp r8, 8
  je  @Copy8
  cmp r8, 9
  je  @Copy9
  cmp r8, 10
  je  @Copy10
  cmp r8, 11
  je  @Copy11
  cmp r8, 12
  je  @Copy12
  cmp r8, 13
  je  @Copy13
  cmp r8, 14
  je  @Copy14
  cmp r8, 15
  je  @Copy15

  ; fall through - 1 byte
  mov al, byte ptr [rdx]
  mov byte ptr [rcx], al
  jmp @RestoreExit

  @Copy2:
  mov r10w, word ptr [rdx]
  mov word ptr [rcx], r10w
  jmp @RestoreExit

  @Copy3:
  mov r10w, word ptr [rdx]
  mov word ptr [rcx], r10w
  mov al, byte ptr [rdx + 02h]
  mov byte ptr [rcx + 02h], al
  jmp @RestoreExit

  @Copy4:
  mov r9d, dword ptr [rdx]
  mov dword ptr [rcx], r9d
  jmp @RestoreExit

  @Copy5:
  mov r9d, dword ptr [rdx      ]
  mov al ,  byte ptr [rdx + 04h]
  mov dword ptr [rcx      ], r9d
  mov  byte ptr [rcx + 04h], al
  jmp @RestoreExit

  @Copy6:
  mov r9d , dword ptr [rdx      ]
  mov r10w,  word ptr [rdx + 04h]
  mov dword ptr [rcx      ], r9d
  mov  word ptr [rcx + 04h], r10w
  jmp @RestoreExit

  @Copy7:
  mov r9d , dword ptr [rdx      ]
  mov r10w,  word ptr [rdx + 04h]
  mov al  ,  byte ptr [rdx + 06h]
  mov dword ptr [rcx      ], r9d
  mov  word ptr [rcx + 04h], r10w
  mov  byte ptr [rcx + 06h], al
  jmp @RestoreExit

  @Copy8:
  mov r8, qword ptr [rdx]
  mov qword ptr [rcx], r8
  jmp @RestoreExit

  @Copy9:
  mov r8, qword ptr [rdx      ]
  mov al,  byte ptr [rdx + 08h]
  mov qword ptr [rcx      ], r8
  mov  byte ptr [rcx + 08h], al
  jmp @RestoreExit

  @Copy10:
  mov r8  , qword ptr [rdx      ]
  mov r10w,  word ptr [rdx + 08h]
  mov qword ptr [rcx      ], r8
  mov  word ptr [rcx + 08h], r10w
  jmp @RestoreExit

  @Copy11:
  mov r8  , qword ptr [rdx      ]
  mov r10w,  word ptr [rdx + 08h]
  mov al  ,  byte ptr [rdx + 0Ah]
  mov qword ptr [rcx      ], r8
  mov  word ptr [rcx + 08h], r10w
  mov  byte ptr [rcx + 0Ah], al
  jmp @RestoreExit

  @Copy12:
  mov r8 , qword ptr [rdx      ]
  mov r9d, dword ptr [rdx + 08h]
  mov qword ptr [rcx      ], r8
  mov dword ptr [rcx + 08h], r9d
  jmp @RestoreExit

  @Copy13:
  mov r8 , qword ptr [rdx      ]
  mov r9d, dword ptr [rdx + 08h]
  mov al ,  byte ptr [rdx + 0Ch]
  mov qword ptr [rcx      ], r8
  mov dword ptr [rcx + 08h], r9d
  mov  byte ptr [rcx + 0Ch], al
  jmp @RestoreExit

  @Copy14:
  mov r8  , qword ptr [rdx      ]
  mov r9d , dword ptr [rdx + 08h]
  mov r10w,  word ptr [rdx + 0Ch]
  mov qword ptr [rcx      ], r8
  mov dword ptr [rcx + 08h], r9d
  mov  word ptr [rcx + 0Ch], r10w
  jmp @RestoreExit

  ; copy 15
  @Copy15:
  mov r8  , qword ptr [rdx + 00h]
  mov r9d , dword ptr [rdx + 08h]
  mov r10w,  word ptr [rdx + 0Ch]
  mov al  ,  byte ptr [rdx + 0Eh]
  mov qword ptr [rcx + 00h], r8
  mov dword ptr [rcx + 08h], r9d
  mov  word ptr [rcx + 0Ch], r10w
  mov  byte ptr [rcx + 0Eh], al
  jmp @RestoreExit

memcpySSE endp
end