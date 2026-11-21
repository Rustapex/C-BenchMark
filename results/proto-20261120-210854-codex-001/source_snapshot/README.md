# 단어 사전 C17 프로토타입

이 폴더에는 세 자료구조의 작은 정확성 검증용 콘솔 프로그램이 있다. 공식 성능 측정, `words.txt` 로더, 난수 작업 목록은 아직 구현하지 않았다. 메뉴 3은 `NOT_IMPLEMENTED`를 출력하며 측정 CSV를 만들지 않는다.

## 열기와 빌드

1. Visual Studio Community에서 `C:\Github\C-BenchMark\prototype\WordDictionaryPrototype.sln`을 연다.
2. 상단에서 `Debug`와 `x64`를 선택하고 **빌드 → 솔루션 빌드**를 누른다. 출력 창의 오류 0개를 확인한다.
3. `WordDictionaryPrototype`이 시작 프로젝트인지 확인한 뒤 `Ctrl+F5`로 실행한다.
4. `Release`와 `x64`로 전환해 다시 빌드한다. Release 프로젝트 설정은 `/std:c17 /TC /utf-8 /O2`다. Debug는 `/Od`와 `DICT_TEST_HOOKS`를 사용한다.

실행 파일은 `build\x64\Debug\WordDictionaryPrototype.exe` 또는 `build\x64\Release\WordDictionaryPrototype.exe`다. Visual Studio에서 파일을 수정했다면 빌드 실패 후 오래된 exe를 실행하지 않는다.

## 메뉴

| 메인 | 기능 |
|---|---|
| `1` | 구조 선택(`1` 리스트, `2` AVL, `3` 해시) → 양의 `initial_n` 입력 → 빈 사전 대화형 실행 |
| `2` | Debug는 core+진단 검사, Release는 core 검사. 마지막 `TEST_SUMMARY`의 `failed=0`, `checks>0`, `exit=0` 확인 |
| `3` | 공식 성능 측정 미구현 안내. 측정 결과는 생성하지 않음 |
| `0` | 종료 |

대화형에서는 `1` 추가, `2` 검색, `3` 삭제, `4` 사전순 출력, `5` 개수·설정, `0` 돌아가기다. 삽입은 소문자 ASCII 3~15자만 허용한다. 검색·삭제는 정상 단어 또는 정상 단어 뒤의 `#` 하나를 허용한다. 해시는 선택한 `initial_n`에 대해 `m=2n`을 생성 시 고정하고 재해싱하지 않는다.

## 기록을 남기는 재현 실행

Windows PowerShell에서 다음 명령을 한 번 실행하면 새 `results\proto-날짜시각-actor-번호\` 폴더가 생긴다. 스크립트는 새 Debug/Release 빌드, 해당 exe의 정확성 검사, 소스 사본과 해시, `build.log`, `accuracy.log`, `console.log`, `manifest.json`을 연결한다. 기존 결과 폴더를 덮어쓰지 않는다.

```powershell
cd C:\Github\C-BenchMark\prototype
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_proto.ps1 -Configuration Debug -Actor user
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_proto.ps1 -Configuration Release -Actor user
```

`ExecutionPolicy Bypass`는 위 **한 프로세스의 실행 인수**이며 시스템 정책을 변경하지 않는다. 스크립트를 실행하지 않고 Visual Studio에서 메뉴를 누른 결과는 별도 수동 확인으로 기록한다. 같은 결과를 주장하려면 새 소스·설정·exe 식별값과 테스트 로그를 대조한다.

콘솔 CLI도 사용할 수 있다.

```powershell
.\build\x64\Debug\WordDictionaryPrototype.exe --test all
.\build\x64\Release\WordDictionaryPrototype.exe --test core
```

Debug의 `all`은 구조 검사·할당 실패·검사기 결함 탐지를 포함한다. Release의 `core`는 같은 자료구조 구현 함수와 정상 사례를 실행하며 Debug 전용 할당 주입은 사용하지 않는다. `--test all`을 Release에서 요청하면 입력 오류로 종료한다.

## 결과 판정

- `manifest.json`의 `status=COMPLETED`와 `console.log`의 `TEST_SUMMARY ... failed=0 ... exit=0`, `accuracy.log`의 실제 시험 행을 함께 확인한다.
- 계획된 반환값·개수·정렬 집합·구조 조건은 `accuracy.log`의 `expected`, `actual`, `result` 열에서 비교한다. 의도적으로 결함을 넣어 탐지한 행은 `EXPECTED_FAILURE_DETECTED`로 표기한다.
- `raw.csv`와 `summary.csv`는 공식 측정 단계 전에는 없다. 현재 결과는 시간 순위나 최종 제출 완료를 뜻하지 않는다.
- 프로그램 종료 코드: 0 성공, 2 인수/입력, 3 정확성, 4 메모리, 6 저장, 8 내부 오류. OS 강제 종료는 이 코드가 보장되지 않는다.

사용자의 Visual Studio 화면 순서와 사례별 입력·예상 결과는 강의 보관함의 `자료구조/과제/프로토타입` 문서를 참조한다.
