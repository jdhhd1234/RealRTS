추천 레이아웃
상단: 작전 상태 바
- 현재 시각 / Day
- 배속
- 진영 상태
- 전체 보급률
- 전체 경보 수
- 선택된 모드(작전, 보급, 영토, 파르티잔, 항공)
예:
- D+2 11:26
- SITREP: 3 Alerts
- Supply: 82%
- Mode: Logistics Overlay
이런 상단 바가 있으면 바로 C2 느낌이 남.
좌측: ORBAT / Task / Filters
좌측은 지금처럼 자유 텍스트 덤프가 아니라 트리형 패널이 좋아.
탭 1: ORBAT
- I Corps
  - 2nd Armored Division
  - 5th Infantry Division
- Rear Command
  - Security Group West
  - Rail Engineers
탭 2: Tasks
- Attack Axis Alpha
- Secure Rail Junction
- Rear Area Security
- Convert Rail Gauge
탭 3: Layers
- Military control
- Legal territory
- Supply network
- Rail damage
- Partisan risk
- Air coverage
이렇게 하면 “게임 오브젝트”가 아니라 실제 지휘 구조를 다루는 느낌이 생겨.
중앙: 공통상황도(COP) 메인 맵
이건 네 화면에서 이미 기반은 있음.
근데 여기에 오버레이와 심볼 체계를 더해야 함.

필수 요소:
- NATO APP-6 부호
- 이동 경로
- 작전 화살표 / phase line
- FEBA(전선), AO(Area of Operations)
- 보급선 색상
- 위험 지역 음영
- 점령/통제 반투명 오버레이
- 중요 거점(HQ, Depot, Airfield, Port, Railhead)
즉, 그냥 “부대가 지도 위에 있음”이 아니라
“작전도 위에 지휘 요소가 겹쳐 있음” 처럼 보여야 해.
우측: 선택 객체 상세 패널
지금 좌측에 몰아둔 정보를 오른쪽 “컨텍스트 패널”로 보내는 게 좋아.

예를 들어 유닛 선택 시:
Unit Detail — 2nd Armored Division
- Side: Blue
- Type: Armor
- HQ: I Corps
- Posture: Defend
- Mission: Hold Center Province
- Strength: 10.0 / 10.0
- Org: 100%
- Fuel: 400 / 400
- Ammo: 600 / 600
- Food: 300 / 300
- Supply Status: Connected
- Estimated Threat: Medium
- Last Contact: None
하단 버튼
- Move
- Attack
- Defend
- Rear Security
- Resupply Priority
- Attach Engineers
즉 정보 + 행동이 같이 있어야 해.
C2 프로그램은 “읽기 화면”이 아니라 “결정·명령 화면”이니까.
하단: 이벤트 / 메시지 / 시간축
지금 이벤트 로그는 맞는 방향이긴 한데, 더 구조화해야 함.

탭:
- Alerts
- Event Log
- Combat Log
- Supply Log
- Intelligence
그리고 색 구분:
- 빨강: 긴급
- 주황: 주의
- 파랑: 정보
- 회색: 일반 로그
예:
- [ALERT] Red Front Railhead isolated
- [WARN] 2nd Arm Div ammo projected below 30% in 6h
- [INFO] New Division request denied: insufficient equipment
이렇게 되면 훨씬 C2 같아져.
3. C2 느낌을 내는 핵심은 “숫자”보다 “상태”야
지금은 숫자가 직접 나열돼 있는데, C2 느낌은 상태 요약이 먼저 보여야 해.

예를 들면:
지금 방식
- Fuel 400
- Ammo 600
- Food 300
더 좋은 방식
- Supply: GREEN
- Fuel: 100%
- Ammo: 100%
- Sustainment Forecast: 48h
혹은
- Combat Ready: YES
- Mobility Limited: NO
- Isolation Risk: LOW
즉, 숫자는 상세창에 넣고
메인 화면에서는 판단용 의미를 먼저 보여줘야 해.
4. “진짜 C2 느낌”을 주는 시각 요소
이건 분위기 쪽이야.
색 체계
- 배경: 어두운 회색/청흑
- 아군: desaturated blue/cyan
- 적군: muted red
- 중립/민간: beige/gray
- 경고: amber/yellow
- 선택: 밝은 노랑 또는 시안
중요한 건 게임처럼 화려하면 안 되고,
실무 도구처럼 차분한 대비가 좋아.
폰트
- 콘솔 느낌 고정폭도 괜찮지만, 전부 고정폭은 너무 디버그툴 같아짐
- 추천:
  - 헤더/코드성 데이터: 고정폭
  - 일반 UI: 읽기 좋은 산세리프
즉 혼합이 좋음.
패널 스타일
- 살짝 얇은 테두리
- 반투명 패널
- 탭 구조
- 카드형 섹션
- 지나치게 둥근 모서리 X
- 과한 네온 효과 X
목표는 “사이버펑크”가 아니라 참모본부/작전실 느낌.
5. 네 게임에 특히 잘 맞는 C2 기능
네 게임은 보급, 파르티잔, 영토가 중요하니까 이런 UI가 엄청 중요해.
A. Logistics Overlay
지도에서 바로:
- 철도 용량
- 도로 용량
- 손상된 간선
- 차단된 간선
- Depot 재고
- 각 부대 예상 보급 지속 시간
이건 네 게임의 정체성 그 자체라서
그냥 수치창보다 지도 오버레이가 훨씬 중요해.
B. Territory / Occupation Overlay
- 법적 영토
- 군사 통제
- 점령지
- 저항 위험도
- 동화 진행도
이건 일반 RTS와 차별화되는 핵심이니까
아예 전용 토글 모드를 두는 게 좋아.
C. Rear Area Security / Partisan Overlay
- 위험도 Low / Medium / High
- 최근 사보타주 위치
- 철도 위협 구간
- 후방 경비 부족 지역
이런 게 있어야 진짜 “후방 관리하는 C2” 느낌이 난다.
D. Orders / Mission system
유닛에 단순 Move만 주면 C2 느낌이 약해.
대신:
- Hold
- Screen
- Delay
- Attack
- Pursue
- Secure Line
- Guard Depot
- Rail Repair
- Area Security
이런 작전형 명령명 이 들어가야 해.
6. 가장 중요한 것: “지휘 흐름”을 만들어야 함
C2 느낌은 UI 스킨이 아니라 작업 흐름이야.

플레이어 흐름이 이런 식이면 좋음:
1. 상황도 확인
2. 경보 확인
3. 문제 지역 선택
4. 관련 부대/거점 선택
5. 명령 발행
6. 결과 추적
즉 UI가
- “정보를 많이 보여주는 것”
  이 아니라
- “결심을 돕는 것”
  이어야 해.
7. 네 현재 화면을 기준으로 당장 바꾸면 좋은 것
우선순위 순으로 말하면:
1단계 — 바로 효과 큰 것
1. 상단 작전 바 추가
2. 좌측 텍스트를 ORBAT/Tasks/Layers 탭으로 분리
3. 우측 선택 객체 상세 패널 추가
4. 하단 Alerts/Event Log 분리
5. 맵 오버레이 토글 추가
이 다섯 개만 해도 체감이 엄청 달라질 거야.
2단계 — C2 느낌 강화
6. NATO APP-6 심볼 정식 적용
7. 작전 화살표 / phase line / objective marker
8. 보급망 시각화
9. 파르티잔 위험도 히트맵
10. 지역/점령 상태 전용 모드
3단계 — “진짜 시스템” 느낌
11. 임무 기반 명령 패널
12. 경보 우선순위 체계
13. 예측 패널
    - “이 부대는 12시간 뒤 탄약 부족”
    - “이 철도 파손은 전선 보급률 15% 감소 유발”
14. 리플레이/시간축
15. 상황 보고(SITREP) 패널
8. 한 줄로 요약하면
지금 UI는 “시뮬레이션 디버그 화면”에 가깝고,
네가 원하는 건 “작전 상황도 중심의 지휘 통제 워크스테이션”이야.

그러려면
- 정보 나열을 줄이고
- 상황도 중심
- 계층적 패널 구조
- 오버레이 기반 판단
- 명령 중심 인터랙션
  으로 바꿔야 해.