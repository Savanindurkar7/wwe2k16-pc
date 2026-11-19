test_vaddsws_1:
  # Lane 0 is a non-overflowing sum whose overflow-detect word has bit 7 set
  # (0x158 + 0x160); lanes 1/2 saturate high/low; lane 3 mixes signs.
  #_ REGISTER_IN v3 [00000158, 7FFFFFF0, 80000010, FFFFFF00]
  #_ REGISTER_IN v4 [00000160, 00000100, FFFFFF00, 00000200]
  vaddsws v3, v3, v4
  blr
  #_ REGISTER_OUT v3 [000002B8, 7FFFFFFF, 80000000, 00000100]
  #_ REGISTER_OUT v4 [00000160, 00000100, FFFFFF00, 00000200]
