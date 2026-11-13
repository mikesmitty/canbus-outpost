// The partition table block picotool makes from partition_table.json at
// build time (CMakeLists.txt), for partition_update.c to install

    .section .rodata.partition_table_block, "a"
    .balign 4
    .global partition_table_block
partition_table_block:
    .incbin PARTITION_TABLE_BIN
1:
    .balign 4
    .global partition_table_block_size
partition_table_block_size:
    .word 1b - partition_table_block
