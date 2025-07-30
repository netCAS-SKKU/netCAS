#!/bin/bash

# Check if script is run as root
if [ "$EUID" -ne 0 ]; then 
    echo "root 권한으로 실행해주세요 (sudo)"
    exit 1
fi

# OpenCAS 인스턴스 정리
echo "OpenCAS 인스턴스 정리 중..."
casadm -P | grep "Cache ID" | awk '{print $3}' | while read -r cache_id; do
    echo "Cache instance $cache_id 제거 중..."
    casadm -S -i "$cache_id" 2>/dev/null
done

# PMEM 디바이스 확인
if [ ! -e "/dev/pmem0" ]; then
    echo "PMEM 디바이스(/dev/pmem0)를 찾을 수 없습니다."
    exit 1
fi

echo "PMEM 디바이스 전체 초기화 중... (10GB)"

# PMEM 디바이스 전체 초기화 (10GB = 10240MB)
if dd if=/dev/zero of=/dev/pmem0 bs=1M count=10240 conv=fsync status=progress; then
    echo "PMEM 전체 초기화 완료"
else
    echo "PMEM 초기화 실패"
    exit 1
fi

# dmesg에서 PMEM 관련 메시지 확인
echo "PMEM 상태 확인 중..."
dmesg | grep -i pmem | tail -n 5

echo "PMEM 초기화가 완료되었습니다."
echo "이제 setup_opencas_pmem.sh를 다시 실행할 수 있습니다." 