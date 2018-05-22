.code
memcpySSE proc
  ; dst = rcx
  ; src = rdx
  ; len = r8

  mov  rax, rcx

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
  and r9 , 0FFFFFFFFFFFFFF80h
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
  jz   @FinalBytes

  mov  r10, 7
  sub  r10, r11
  imul r10, 10
  lea  r9 , @FinalBlocks
  add  r9 , r10
  jmp  r9

  @RestoreExit:
  movdqa xmm6 , oword ptr [rsp + 4*8 + 00]
  movdqa xmm7 , oword ptr [rsp + 4*8 + 16]
  add rsp, 8 + 2*16 + 4*8

  @Exit:
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

  movdqa xmm6 , oword ptr [rsp + 4*8 + 00]
  movdqa xmm7 , oword ptr [rsp + 4*8 + 16]
  add rsp, 8 + 2*16 + 4*8
  sfence

  shl  r11, 4
  add  rdx, r11
  add  rcx, r11

  @FinalBytes:
  and r8, 0Fh
  jz  @Exit
  imul r8, 5
  lea  r9, @FinalBytesTable
  add  r9, r8
  jmp  r9

  @FinalBytesTable:
  jmp @Copy1
  jmp @Copy2
  jmp @Copy3
  jmp @Copy4
  jmp @Copy5
  jmp @Copy6
  jmp @Copy7
  jmp @Copy8
  jmp @Copy9
  jmp @Copy10
  jmp @Copy11
  jmp @Copy12
  jmp @Copy13
  jmp @Copy14
  jmp @Copy15

  db 128 DUP(0CCh)

  ; fall through - 1 byte
  @Copy1:
  mov al, byte ptr [rdx]
  mov byte ptr [rcx], al
  ret

  @Copy2:
  mov r10w, word ptr [rdx]
  mov word ptr [rcx], r10w
  ret

  @Copy3:
  mov r10w, word ptr [rdx]
  mov word ptr [rcx], r10w
  mov r11b, byte ptr [rdx + 02h]
  mov byte ptr [rcx + 02h], r11b
  ret

  @Copy4:
  mov r9d, dword ptr [rdx]
  mov dword ptr [rcx], r9d
  ret

  @Copy5:
  mov r9d, dword ptr [rdx      ]
  mov r11b ,  byte ptr [rdx + 04h]
  mov dword ptr [rcx      ], r9d
  mov  byte ptr [rcx + 04h], r11b
  ret

  @Copy6:
  mov r9d , dword ptr [rdx      ]
  mov r10w,  word ptr [rdx + 04h]
  mov dword ptr [rcx      ], r9d
  mov  word ptr [rcx + 04h], r10w
  ret

  @Copy7:
  mov r9d , dword ptr [rdx      ]
  mov r10w,  word ptr [rdx + 04h]
  mov r11b,  byte ptr [rdx + 06h]
  mov dword ptr [rcx      ], r9d
  mov  word ptr [rcx + 04h], r10w
  mov  byte ptr [rcx + 06h], r11b
  ret

  @Copy8:
  mov r8, qword ptr [rdx]
  mov qword ptr [rcx], r8
  ret

  @Copy9:
  mov r8  , qword ptr [rdx      ]
  mov r11b,  byte ptr [rdx + 08h]
  mov qword ptr [rcx      ], r8
  mov  byte ptr [rcx + 08h], r11b
  ret

  @Copy10:
  mov r8  , qword ptr [rdx      ]
  mov r10w,  word ptr [rdx + 08h]
  mov qword ptr [rcx      ], r8
  mov  word ptr [rcx + 08h], r10w
  ret

  @Copy11:
  mov r8  , qword ptr [rdx      ]
  mov r10w,  word ptr [rdx + 08h]
  mov r11b,  byte ptr [rdx + 0Ah]
  mov qword ptr [rcx      ], r8
  mov  word ptr [rcx + 08h], r10w
  mov  byte ptr [rcx + 0Ah], r11b
  ret

  @Copy12:
  mov r8 , qword ptr [rdx      ]
  mov r9d, dword ptr [rdx + 08h]
  mov qword ptr [rcx      ], r8
  mov dword ptr [rcx + 08h], r9d
  ret

  @Copy13:
  mov r8  , qword ptr [rdx      ]
  mov r9d , dword ptr [rdx + 08h]
  mov r11b,  byte ptr [rdx + 0Ch]
  mov qword ptr [rcx      ], r8
  mov dword ptr [rcx + 08h], r9d
  mov  byte ptr [rcx + 0Ch], r11b
  ret

  @Copy14:
  mov r8  , qword ptr [rdx      ]
  mov r9d , dword ptr [rdx + 08h]
  mov r10w,  word ptr [rdx + 0Ch]
  mov qword ptr [rcx      ], r8
  mov dword ptr [rcx + 08h], r9d
  mov  word ptr [rcx + 0Ch], r10w
  ret

  ; copy 15
  @Copy15:
  mov r8  , qword ptr [rdx + 00h]
  mov r9d , dword ptr [rdx + 08h]
  mov r10w,  word ptr [rdx + 0Ch]
  mov r11b,  byte ptr [rdx + 0Eh]
  mov qword ptr [rcx + 00h], r8
  mov dword ptr [rcx + 08h], r9d
  mov  word ptr [rcx + 0Ch], r10w
  mov  byte ptr [rcx + 0Eh], r11b
  ret

memcpySSE endp
end