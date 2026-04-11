#####################################################################
# CS:APP Malloc Lab
# 한국어 정리본
#
# Copyright (c) 2002, R. Bryant and D. O'Hallaron, All rights reserved.
# May not be used, modified, or copied without permission.
#
######################################################################

***********
주요 파일
***********

mm.{c,h}
	학생이 구현해야 하는 malloc 패키지입니다. 실제로 제출하고 수정하는 핵심 파일은 `mm.c`입니다.

mdriver.c
	`mm.c`를 테스트하는 드라이버 프로그램입니다.

short{1,2}-bal.rep
	디버깅을 시작할 때 쓰기 좋은 아주 작은 trace 파일입니다.

Makefile
	드라이버와 관련 파일을 빌드합니다.

**********************
드라이버 지원 파일
**********************

config.h	드라이버 설정 파일
fsecs.{c,h}	여러 타이머 패키지를 감싸는 래퍼 함수
clock.{c,h}	사이클 카운터 접근 루틴
fcyc.{c,h}	사이클 카운터 기반 타이머 함수
ftimer.{c,h}	interval timer/gettimeofday 기반 타이머 함수
memlib.{c,h}	힙과 `sbrk` 동작을 모의하는 라이브러리

************************
빌드 및 기본 실행 방법
************************

드라이버를 빌드하려면 셸에서 다음을 실행합니다.

	unix> make

작은 trace 파일 하나로 빠르게 테스트하려면 다음과 같이 실행합니다.

	unix> mdriver -V -f short1-bal.rep

여러 trace 파일을 한 번에 모두 실행하려면 다음과 같이 입력할 수 있습니다.

	unix> for file in traces/*.rep; do echo "Running test: $file"; ./mdriver -f "$file"; done

사용 가능한 옵션 목록을 보려면 다음을 실행합니다.

	unix> mdriver -h

****************
옵션 설명
****************

- `-a`: `team` 정보 구조체 검사를 생략합니다. 팀 정보 확인이 필요 없을 때 사용합니다.
- `-f <file>`: 특정 trace 파일 하나를 지정해 실행합니다. 원하는 trace만 골라서 테스트할 때 유용합니다.
- `-g`: autograder용 요약 정보만 출력합니다. 자동 채점 환경에서 필요한 간단한 결과 형식입니다.
- `-h`: 사용 가능한 옵션 목록을 출력합니다.
- `-l`: 학생이 구현한 `mm.c`뿐 아니라 시스템의 `libc malloc`도 함께 실행해 비교합니다.
- `-t <dir>`: 기본 trace 파일들을 찾을 디렉터리를 지정합니다.
- `-v`: 각 trace 파일에 대한 성능 분석 결과를 자세히 출력합니다. 예를 들어 `util`, `ops`, `secs`, `Kops` 같은 지표를 trace별로 확인할 수 있습니다.
- `-V`: `-v`보다 더 상세한 정보를 출력합니다. 각 단계의 진행 상황이나 추가 디버깅 정보까지 보고 싶을 때 유용합니다.

****************
출력 항목 설명
****************

- `util`: 트레이스 실행 동안 측정된 공간 활용률입니다. 실제 할당된 블록의 최대 총 크기를 시뮬레이션 힙 크기로 나눈 값입니다.
- `ops`: 처리한 요청 수입니다. `malloc`/`free`/`realloc` 호출의 총 개수를 뜻합니다.
- `secs`: 해당 trace 실행에 걸린 시간(초)입니다.
- `Kops`: 초당 처리한 요청 수를 천 단위로 나타낸 값입니다. 계산식은 `(ops / 1000) / secs`입니다.

************************
Trace 디렉터리 개요
************************

`traces/` 디렉터리에는 학생이 작성한 malloc 패키지를 평가하기 위해 테스트 하네스가 사용하는 allocate/free 요청 trace들이 들어 있습니다.

파일 구성:

- `*.rep`: 원본 trace 파일
- `*-bal.rep`: 균형이 맞는(balanced) 버전의 trace 파일
- `gen_XXX.pl`: `*.rep` 파일을 생성하는 Perl 스크립트
- `checktrace.pl`: trace의 일관성을 검사하고 balanced 버전을 생성하는 스크립트
- `Makefile`: trace들을 생성하는 빌드 파일

참고로 balanced trace는 각 allocate 요청마다 대응되는 free 요청이 존재하는 trace를 뜻합니다.

trace를 처음부터 다시 생성하려면 다음을 입력하세요.

	unix> make

****************
Trace 파일 형식
****************

trace 파일은 ASCII 파일이며, 시작 부분은 다음 4줄 헤더로 구성됩니다.

```text
<sugg_heapsize>   /* 권장 힙 크기 (사용되지 않음) */
<num_ids>         /* 요청 id의 개수 */
<num_ops>         /* 요청(연산) 개수 */
<weight>          /* 이 trace의 가중치 (사용되지 않음) */
```

헤더 뒤에는 `num_ops`개의 텍스트 줄이 이어지며, 각 줄은 allocate(`a`), reallocate(`r`), free(`f`) 요청 중 하나를 나타냅니다.

```text
a <id> <bytes>  /* ptr_<id> = malloc(<bytes>) */
r <id> <bytes>  /* realloc(ptr_<id>, <bytes>) */
f <id>          /* free(ptr_<id>) */
```

예를 들어 다음 trace 파일은:

```text
20000
3
8
1
a 0 512
a 1 128
r 0 640
a 2 128
f 1
r 0 768
f 0
f 2
```

balanced trace입니다. 권장 힙 크기 `20000`은 무시되며, 서로 다른 요청 id는 `0`, `1`, `2`의 3개이고, 전체 요청 수는 8개입니다.

****************
대표 trace 설명
****************

- `short{1,2}-bal.rep`: 디버깅을 위한 아주 작은 synthetic trace 파일입니다.
- `{amptjp,cccp,cp-decl,expr}-bal.rep`: 실제 프로그램으로부터 생성된 trace들입니다.
- `{binary,binary2}-bal.rep`: 작은 블록과 큰 블록을 번갈아 할당하는 패턴으로, 교차 크기 요청에서의 배치와 단편화를 보기 좋습니다.
- `coalescing-bal.rep`: 인접 free 블록이 실제로 coalescing되는지 직접적으로 테스트합니다.
- `{random,random2}-bal.rep`: 무작위 allocate/free 요청으로 정확성과 견고성을 전반적으로 테스트합니다.
- `{realloc,realloc2}-bal.rep`: realloc 구현이 효율적인지, 불필요한 복사나 내부 단편화가 심하지 않은지 확인합니다.

***************************
기본 테스트케이스 순서
***************************

`mdriver`가 사용하는 기본 trace 순서는 다음과 같습니다.

```text
0: amptjp-bal.rep
1: cccp-bal.rep
2: cp-decl-bal.rep
3: expr-bal.rep
4: coalescing-bal.rep
5: random-bal.rep
6: random2-bal.rep
7: binary-bal.rep
8: binary2-bal.rep
9: realloc-bal.rep
10: realloc2-bal.rep
```

************************
기본 테스트케이스 의미
************************

- `amptjp-bal.rep`: 실제 프로그램에서 수집한 워크로드로, allocator가 현실적인 `malloc`/`free` 흐름에서도 잘 동작하는지 봅니다.
- `cccp-bal.rep`: 다른 allocation 리듬과 블록 수명 패턴 아래에서 정확성과 utilization을 확인합니다.
- `cp-decl-bal.rep`: 또 다른 실제 workload로, 여러 현실적 패턴에서 안정적으로 동작하는지 확인합니다.
- `expr-bal.rep`: 다양한 요청 크기에서 block splitting, alignment, free-list 재사용이 제대로 되는지 확인합니다.
- `coalescing-bal.rep`: 인접 free 블록이 합쳐지고, 합쳐진 공간이 다시 재사용 가능한지 검사합니다.
- `random-bal.rep`: 다양한 크기의 무작위 연산으로 정확성, 단편화, 메모리 재사용 능력을 넓게 테스트합니다.
- `random2-bal.rep`: 다른 랜덤 패턴으로, 특정 랜덤 케이스에만 우연히 잘 맞는 구현을 걸러냅니다.
- `binary-bal.rep`: 작은 요청과 큰 요청을 번갈아 처리할 때 배치 정책이 안정적인지 확인합니다.
- `binary2-bal.rep`: 또 다른 교차 크기 패턴으로, 반복 압박이 쌓일 때의 단편화 문제를 드러냅니다.
- `realloc-bal.rep`: `realloc`이 효율적으로 구현되어 있는지, 과도한 복사와 단편화가 없는지 테스트합니다.
- `realloc2-bal.rep`: 점진적으로 커지는 `realloc` 패턴에서 순진한 재할당 기반 구현의 약점을 잘 드러냅니다.
