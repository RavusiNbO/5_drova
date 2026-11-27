savedcmd_/root/drivers/5_drova/edu_dma.mod := printf '%s\n'   edu_dma.o | awk '!x[$$0]++ { print("/root/drivers/5_drova/"$$0) }' > /root/drivers/5_drova/edu_dma.mod
