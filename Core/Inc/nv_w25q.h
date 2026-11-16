/* nvlog 를 외부 플래시 W25Q128 위에 올리는 연결 */
#pragma once

#include "nvlog.h"
#include "w25q.h"

void nv_w25q_ops(flash_ops_t *ops, const spi_dev_t *dev);
