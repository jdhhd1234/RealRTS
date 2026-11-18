# QGIS 지도 제작 가이드 (RealRTS)

이 문서는 RealRTS가 읽을 수 있는 지도 데이터를 QGIS로 만드는 방법을 설명한다.
사람이나 다른 AI가 이 문서만 보고 지도를 만들 수 있도록 썼다.
규칙의 원본은 `game/crates/mapbake/src/lib.rs`와 `architect.md` 5.4이다. 내용이 다르면 코드가 맞다.

---

## 1. 결과물

아래 둘 중 하나를 만든다.

| 형식 | 구성 |
|---|---|
| **GeoPackage (권장)** | `.gpkg` 파일 하나에 레이어 여러 개 |
| **GeoJSON / Shapefile 폴더** | 폴더 하나에 레이어마다 파일 하나: `terrain.shp`, `rail.geojson` 등. 두 형식을 섞어도 된다 |

게임에서 여는 방법:

- 게임 안: 상단 `File` → `Open map...` → `.gpkg`를 고르거나, 폴더 안 파일 중 아무거나 하나를 고른다(폴더 전체를 읽는다).
- 명령줄: `game/`에서 `cargo run -p app -- <경로>`
- 검증만 하기: `game/`에서 `cargo run -p mapbake -- <경로> out.ron`
  - 성공하면 크기, 지역 수, 노드 수, 간선 수, 부대 수를 출력한다.
  - 실패하면 오류 메시지를 출력한다(8절 참고).

---

## 2. 반드시 지킬 규칙

1. **좌표계는 미터 단위 투영 좌표계여야 한다.**
   - `EPSG:4326`(위경도, 도 단위)은 거부된다.
   - 보통 지역의 UTM 존을 쓴다:
     - 북반구는 `EPSG:326xx`, 남반구는 `EPSG:327xx` (`xx` = 존 번호)
     - 존 번호 = (경도 + 180) ÷ 6 의 소수점을 버리고 + 1
   - 예: 한반도 `EPSG:32652` 또는 `EPSG:5179`, 우크라이나 중부 `EPSG:32636`, 독일 `EPSG:32632`
2. **모든 레이어가 같은 좌표계여야 한다.**
3. **레이어 이름은 정확히 아래 7개 중 하나여야 한다.**
   - `terrain`, `regions`, `nodes`, `rail`, `roads`, `rivers`, `units` (대소문자 무시)
   - 다른 이름의 레이어는 무시된다. 모든 레이어는 선택 사항이지만, 최소 하나는 있어야 한다.
4. **지도 크기는 한 변 최대 2000 km이다.** 범위는 모든 레이어의 모든 도형을 합친 영역이다.
5. **속성(필드) 이름은 아래 표와 정확히 같아야 한다**(대소문자 무시). 표에 없는 필드는 무시된다.
6. 숫자 필드는 정수·실수·숫자로 된 텍스트 모두 된다. 빈 값(NULL)이면 기본값을 쓴다.

---

## 3. 레이어 규약

### 3.1 `terrain` — 지형 (폴리곤)

| 필드 | 값 | 기본값 |
|---|---|---|
| `kind` | `plains` / `forest` / `marsh` / `mountain` / `urban` / `water` | `plains` |

- 1 km 격자로 바뀐다. 셀 중심이 폴리곤 안에 있으면 그 지형이 된다. 구멍(내부 링)도 지원한다.
- 폴리곤이 덮지 않는 곳은 `plains`이다.
- 겹치면 뒤쪽이 이긴다: `plains` < `forest` < `marsh` < `urban` < `mountain` < `water`
- 다른 값(예: `lava`)을 쓰면 오류가 난다.
- 게임에서 미치는 영향:
  - 이동 비용: plains 1, urban 1.5, forest 2, marsh 3, mountain 4, water 10
  - 방어 배율: forest 1.5, marsh 1.2, urban·mountain 2
- 큰 강이나 호수는 `water` 폴리곤으로 그린다. 강을 선으로만 그리면 지형으로 막히지 않는다.

### 3.2 `regions` — 행정 지역 (폴리곤)

| 필드 | 값 | 기본값 |
|---|---|---|
| `name` | 텍스트 | `Region N` |
| `side` | 법적 소유 진영, 정수 0~7 | 0 |
| `control` | 시작 시 군사 통제 진영, 정수 0~7 | `side`와 같음 |
| `fuel` | 하루 연료 생산량 (t) | 0 |
| `ammo` | 하루 탄약 생산량 (t) | 0 |
| `food` | 하루 식량 생산량 (t) | 0 |
| `equipment` | 하루 장비 생산량 (t). 보충병과 신규 사단에 쓰인다 | 0 |

- 진영 번호: **0 = 플레이어(Blue), 1 = 적(Red)**.
- `side ≠ control`이면 시작부터 점령지다. 파르티잔이 활동하고, 생산량은 점령군에게 가지 않는다.
- 지역 폴리곤들이 **지도 전체를 빈틈 없이 덮어야 한다.** 어느 지역에도 속하지 않는 셀은 `Unassigned` 지역(진영 0)으로 묶인다.
  - 지도 가장자리 1줄이 남기 쉬우므로, 지역 폴리곤을 지도 범위보다 약간 크게 그린다.
- 참고 값(테스트 맵): 지역당 연료 0~40, 탄약 0~40, 식량 40~60, 장비 0~20.

### 3.3 `nodes` — 수송 노드 (포인트)

| 필드 | 값 | 기본값 |
|---|---|---|
| `name` | 텍스트 | 없음 |
| `kind` | `junction` / `station` / `port` | `junction` |
| `base_side` | 이 노드에 보급 **기지**를 둘 진영 (0~7) | 없음 |
| `depot_side` | 이 노드에 야전 **보급소**를 둘 진영 (0~7) | 없음 |

- 역, 분기점, 항구, 도시마다 하나씩 찍는다.
- **기지**: 국가 보급이 들어오는 곳이다(품목당 20000 t로 시작). 진영마다 하나 이상 필요하다.
  - 지역 생산량은 그 진영의 첫 번째 기지로 들어간다.
  - 기지가 없는 진영은 보급을 받지 못한다.
- **보급소**: 기지에서 철도·도로로 보급을 받아, 반경 150 km 안의 부대에 트럭으로 나눠준다(50 km까지 효율 100%).
  - 전선 가까이에 둔다.
- 같은 노드에 기지와 보급소를 함께 둘 수 있다.

### 3.4 `rail` / `roads` / `rivers` — 수송선 (라인)

| 레이어 | 필드 | 기본값 |
|---|---|---|
| `rail` | `gauge` (궤간 번호, 정수 0~255), `capacity` (t/일) | gauge 0, capacity 3000 |
| `roads` | `capacity` (t/일) | 600 |
| `rivers` | `capacity` (t/일) | 1500 |

**연결 규칙 (가장 실수하기 쉬운 부분)**

- 선 하나는 **양 끝점만** 사용한다. 중간 꼭짓점은 길이 계산에만 쓰인다.
- 끝점은 **2 km 안의 가장 가까운 노드**에 연결된다. 2 km 안에 노드가 없으면 그 자리에 이름 없는 분기점 노드가 새로 생긴다.
- 따라서 **선은 노드에서 노드까지 한 구간씩 잘라야 한다.**
  - 분기점이나 역을 지나가는 긴 선은 그 노드와 연결되지 않는다.
  - QGIS에서 `Split with lines`나 `Split lines by maximum length`로 자르거나, 처음부터 구간별로 그린다.
- 두 선의 끝점이 2 km 안에 있으면 같은 노드로 합쳐지므로, 노드 레이어 없이도 연결은 된다. 다만 기지와 보급소를 두려면 `nodes`가 필요하다.
- 양 끝이 같은 노드로 붙는 선(2 km보다 짧은 선이나 고리)은 버려진다.
- 서로 다른 `gauge`가 만나는 곳이 궤간 변경 지점이다(나중에 공병 전환 대상).
- 적이 통제하는 노드를 지나는 철도·도로로는 보급이 가지 않는다.

### 3.5 `units` — 시작 부대 (포인트)

| 필드 | 값 | 기본값 |
|---|---|---|
| `name` | 텍스트 | `Unit N` |
| `side` | 진영 0~7 | 0 |

- 모두 표준 사단(보병 6, 기갑 2, 포병 2 대대)으로 생성되고, 보급은 가득 찬 상태로 시작한다.
- 부대 반경 10 km는 그 진영의 군사 통제가 된다. 따라서 적 지역 안에 찍으면 시작부터 그 주변을 점령한다.
- 맵 범위 안에 있어야 한다.

---

## 4. QGIS 작업 순서 (GUI)

1. **프로젝트 좌표계 정하기**
   - `프로젝트 → 속성 → 좌표계`에서 UTM 존을 고른다(2절).
2. **레이어 만들기**
   - `레이어 → 레이어 생성 → 새 GeoPackage 레이어`
   - 데이터베이스: `my_map.gpkg`
   - 테이블 이름: `terrain` 등 규약 이름
   - 도형 유형: 폴리곤 / 포인트 / 라인
   - 좌표계: 1번에서 정한 것
   - 필드: 3절 표대로 추가한다(`side`는 정수, `fuel`은 실수, `name`·`kind`는 텍스트).
3. **그리기**
   - 순서: `regions` → `terrain` → `nodes` → `rail`/`roads` → `units`
   - 선을 그릴 때 스냅을 켠다(`프로젝트 → 스냅 옵션`, 노드 레이어 대상, 꼭짓점). 그러면 끝점이 노드에 정확히 붙는다.
4. **이미 있는 데이터(OSM, 행정구역 shp 등)를 쓸 때**
   - 레이어 우클릭 → `내보내기 → 다른 이름으로 객체 저장`
   - 형식: GeoPackage
   - 파일: 같은 `.gpkg`
   - 레이어 이름: 규약 이름
   - 좌표계: 1번에서 정한 UTM
   - 필요 없는 필드는 빼고, 규약 필드는 `필드 계산기`로 만든다(예: OSM `landuse='forest'` → `kind='forest'`).
5. **검증**
   - `cargo run -p mapbake -- my_map.gpkg out.ron`

> 주의: 프로젝트 좌표계만 바꾸면 파일 안의 좌표는 그대로다. 4번처럼 **다시 저장**해야 한다.

---

## 5. PyQGIS 스크립트 (QGIS Python 콘솔)

기존 레이어들을 규약 이름과 UTM 좌표계로 한 `.gpkg`에 저장한다. `MAPPING`, `TARGET_CRS`, `OUT`만 고치면 된다.

```python
from qgis.core import (QgsProject, QgsVectorFileWriter, QgsCoordinateReferenceSystem,
                       QgsCoordinateTransform, QgsCoordinateTransformContext)

OUT = r"C:/maps/my_map.gpkg"
TARGET_CRS = QgsCoordinateReferenceSystem("EPSG:32652")   # 지역 UTM 존으로 바꾼다
# QGIS 레이어 이름 -> 게임 레이어 이름
MAPPING = {
    "landcover": "terrain",
    "admin_areas": "regions",
    "stations": "nodes",
    "railways": "rail",
    "main_roads": "roads",
    "start_units": "units",
}

first = True
for src_name, dst_name in MAPPING.items():
    layer = QgsProject.instance().mapLayersByName(src_name)[0]
    opts = QgsVectorFileWriter.SaveVectorOptions()
    opts.driverName = "GPKG"
    opts.layerName = dst_name
    if layer.crs() != TARGET_CRS:
        opts.ct = QgsCoordinateTransform(layer.crs(), TARGET_CRS, QgsProject.instance())
    opts.actionOnExistingFile = (QgsVectorFileWriter.CreateOrOverwriteFile if first
                                 else QgsVectorFileWriter.CreateOrOverwriteLayer)
    err = QgsVectorFileWriter.writeAsVectorFormatV3(layer, OUT, QgsCoordinateTransformContext(), opts)
    print(dst_name, err)
    first = False
```

GeoJSON 폴더로 내보내려면 `opts.driverName = "GeoJSON"`으로 바꾸고, 출력 경로를 `폴더/<dst_name>.geojson`으로 하고, 매번 `CreateOrOverwriteFile`을 쓴다.

---

## 6. GeoJSON / Shapefile 주의점

- 파일 이름이 곧 레이어 이름이다: `terrain.geojson`, `rail.shp`
- 같은 레이어 파일이 두 개 있으면 오류가 난다(예: `rail.shp`와 `rail.geojson`).
- Shapefile은 `.shp`, `.shx`, `.dbf`가 모두 있어야 한다.
  - `.prj`가 있으면 그것으로 좌표계를 판단한다.
  - `.cpg`가 있으면 그 문자 인코딩을 따른다(한글 속성은 `.cpg`에 `UTF-8` 또는 `CP949`).
  - Shapefile 필드 이름은 10자 제한이다. `depot_side`(10자)는 되지만 더 긴 이름은 잘린다.
- GeoJSON은 QGIS에서 투영 좌표계로 내보내면 `crs` 항목이 들어간다. 이 항목이 없고 좌표가 모두 ±180/±90 안이면 도 단위로 보고 거부한다.

---

## 7. 완성 체크리스트

- [ ] 모든 레이어가 같은 미터 단위 투영 좌표계(UTM 등)다
- [ ] 레이어 이름이 규약 이름이다
- [ ] `regions`가 지도 전체를 덮는다(가장자리 포함)
- [ ] 진영마다 `base_side`가 있는 노드가 최소 하나 있다
- [ ] 전선 근처에 `depot_side` 노드가 있고, 기지와 철도·도로로 연결된다
- [ ] 철도·도로 선이 노드에서 노드까지 구간별로 잘려 있다
- [ ] 선 끝점이 노드에서 2 km 안에 있다(스냅 사용)
- [ ] `terrain`의 `kind` 값이 6개 중 하나다
- [ ] `side` / `control` / `base_side` / `depot_side`가 정수 0~7이다
- [ ] `mapbake` 검증이 오류 없이 끝나고, 출력된 노드·간선 수가 예상과 맞다

---

## 8. 오류 메시지와 해결

| 메시지 (앞부분) | 원인 | 해결 |
|---|---|---|
| `no known layers found; expected some of: ...` | `.gpkg`에 규약 이름의 레이어가 없다 | 레이어 이름을 `terrain`, `rail` 등으로 다시 저장 |
| `no layer files in ...` | 폴더에 규약 이름의 `.geojson`/`.shp`가 없다 | 파일 이름을 `terrain.shp` 등으로 바꾼다 |
| `CRS 4326 is geographic (degrees)...` / `coordinates are geographic (degrees)...` | 위경도 좌표계다 | UTM 등 투영 좌표계로 다시 저장 |
| `layers have an undefined geographic CRS` | 좌표계가 지정되지 않았다 | 레이어에 좌표계를 지정하고 다시 저장 |
| `layer X uses CRS a, others use b` / `... uses EPSG:a but ... uses EPSG:b` | 레이어마다 좌표계가 다르다 | 모두 같은 좌표계로 다시 저장 |
| `map is W x H km; the limit is 2000 km per side` | 범위가 너무 크거나 단위가 미터가 아니다 | 범위를 줄이거나 좌표계를 확인 |
| `terrain feature N: unknown kind '...'` | `kind` 값 오타 | 6개 값 중 하나로 고친다 |
| `field side: side N must be a whole number 0..7` | 진영 값이 잘못됐다 | 0~7 정수로 고친다 |
| `field X: '...' is not a number` | 숫자 필드에 글자가 들어 있다 | 숫자로 고친다 |
| `unknown kind '...' (use junction, station, port)` | 노드 `kind` 오타 | 고친다 |
| `gauge ... must be a whole number 0..255` / `capacity ... must be positive` | 철도 속성 값 오류 | 고친다 |
| `two files for layer X` | 같은 레이어 파일이 두 개 있다 | 하나를 지운다 |
| `cannot read shapefile (.shp/.shx/.dbf)` | Shapefile 구성 파일이 빠졌다 | `.shx`, `.dbf`를 같이 둔다 |
| `unit X outside the map` | 부대가 지도 범위 밖에 있다 | 위치를 고친다 |

---

## 9. 참고 샘플

- `game/samples/sample.gpkg`: 규약대로 만든 220 × 150 km 가상 지도(EPSG:32635). QGIS에서 열어 레이어 구조와 필드를 확인할 수 있다.
- `game/samples/geojson/`: 같은 지도를 GeoJSON 폴더로 만든 것.
- 생성 스크립트: `game/tools/sample_gpkg.py` (Python 표준 라이브러리만 사용).
