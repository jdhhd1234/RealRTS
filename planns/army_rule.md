주의사항: 여기있는 로직은 엔진코어로직이 아닌 모딩로직으로 별도분리한다.
# 군사단위및 규칙

야전군 (Army)	XXXX	약 10만 ~ 30만 명	대장
군단 (Corps)	XXX	3만 ~ 6만 명	중장
사단 (Division)	XX	1만 ~ 2만 명	소장
여단 (Brigade)	X	4,000 ~ 2만 명	준장 / 여단장
대대 (Regiment / Group)	III	500 ~ 2,000명	대령

# 편재(사단기준)
- 대대를 최소 단위로 한다.
- 여러가지 역할을 담당하는 편제가 있다.

어떤 종류의 대대및 편제가 있을까?

1. 전투부대 대대
• 보병대대 / 기계화보병대대: 대대본부(중박격포 등)와 3개 안팎의 소총(보병)중대로 구성되며, 지상 전투의 기본을 담당합니다.
• 전차대대 / 기갑대대: 대대본부와 여러 개의 전차(기갑)중대로 편성되어 기동 타격과 돌파 임무를 수행합니다.
• 특공대대 / 수색대대: 특수작전, 정찰, 적진 침투 등 고난도 임무에 특화된 편제입니다.

2. 전투지원부대 대대
• 포병대대: 여러 개 포대(포병중대)로 이루어지며 자주포·견인포·다연장로켓 등으로 화력을 지원합니다.
• 공병대대: 장애물 개척, 진지 구축, 도하 및 시설 지원 등을 담당합니다.
• 정보통신대대 / 방공대대 / 화생방대대: 통신망 운용, 대공 방어, 화생방 정찰·제독 등 전문 지원 임무를 맡습니다.

3. 전투근무지원부대 대대
• 군수지원대대 / 보급·수송·정비대대: 탄약·식량·유류 보급, 수송, 장비 정비, 의무 지원 등 부대 유지에 필요한 후방·군수 지원을 수행합니다.

여기있는 대대 말고도 유저가 Lua를 통해 직접 새로운 대대를 만들거나 할수 있다 그리고 보병제식장비나 장갑차종류는 Lua에서 제어한다.

예제
```txt
제1해병연대 / 1st Marine Regiment
제1대대, 제1해병연대 / 1st Battalion, 1st Marine
제2대대, 제1해병연대 / 2nd Battalion, 1st Marine
제3대대, 제1해병연대 / 3rd Battalion, 1st Marine
제1대대, 제4해병연대 / 1st Battalion, 4th Marine
external/img2.wi... 제5해병연대 / 5th Marine Regiment
제1대대, 제5해병연대 / 1st Battalion, 5th Marine
제2대대, 제5해병연대 / 2nd Battalion, 5th Marine
제3대대, 제5해병연대 / 3rd Battalion, 5th Marine
제2대대, 제4해병연대 / / 2nd Battalion, 4th Marine
제7해병연대 / 7th Marine Regiment
제1대대, 제7해병연대 / 1st Battalion, 7th Marine
제2대대, 제7해병연대 / 2nd Battalion, 7th Marine
제3대대, 제7해병연대 / 3rd Battalion, 7th Marine
제3대대, 제4해병연대 / 3rd Battalion, 4th Marine
제11해병연대(포병) / 11th Marine Regiment
제1대대, 제11해병연대 / 1st Battalion, 11th Marine[M777A2]
제2대대, 제11해병연대 / 2nd Battalion, 11th Marine[M777A2]
제3대대, 제11해병연대 / 3rd Battalion, 11th Marine[M777A2]
제5대대, 제11해병연대 / 5th Battalion, 11th Marine[M142]
제1해병사단 본부대대
본부중대
통신중대
트럭중대
군악대
external/img2.wi...제1수색대대 / 1st Reconnaissance Battalion
제1경기갑수색대대 / 1st Light Armored Reconnaissance Battalion[LAV-25]
제3경기갑수색대대 / 3rd Light Armored Reconnaissance Battalion[LAV-25]
제3강습수륙양용대대 / 3rd Assault Amphibian Battalion
external/img1.wi... 제1전투공병대대 / 1st Combat Engineer Battalion
```