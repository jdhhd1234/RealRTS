# Rust → C++20 포팅 규칙 (작업자 공통)

> 이식 완료(2026-11-18). Rust 원본은 git 태그 `rust-backup`에만 있다. C++ 소스 폴더는 이후 `src/`에서 `cpp_/`로 바뀌었다.

참고 코드: `game/crates/` (Rust). 결과물: `game/src/<crate>/`, 테스트: `game/tests/`.
반드시 먼저 읽기: 저장소 루트 `CLAUDE.md`(코드 규칙), `architect.md`(2·3·5.4장), 이미 포팅된 `src/mapdata/mapdata.hpp/.cpp`와 `tests/mapdata_test.cpp`(스타일 기준).

## 이름 규칙 (병렬 작업 후 합치기 위해 필수)
- 네임스페이스 = Rust 크레이트 이름 (`sim`, `mapbake`, `app`). Rust 모듈 하나 = `.hpp` + `.cpp` 한 쌍 (예: `crates/sim/src/supply.rs` → `src/sim/supply.hpp`, `src/sim/supply.cpp`). `lib.rs` → `<crate>.hpp/.cpp`.
- 타입·필드·함수·enum 값 이름은 Rust와 똑같이 (타입은 PascalCase, 함수·필드는 snake_case). 다른 작업자가 Rust 코드만 보고 C++ API를 짐작할 수 있어야 한다.
- `Option<T>` → `std::optional<T>`, `Vec<T>` → `std::vector<T>`, `Result<T, String>` → 반환값 + 실패 시 예외(`std::runtime_error` 상속한 예외 타입만 허용), `HashMap` → 결정론이 필요하면 `std::map`, 아니면 `std::unordered_map`.
- bevy_ecs: `Component` → 평범한 `struct`, `Resource` → `registry.ctx()`, 시스템 → 자유 함수 `void xxx_system(entt::registry&)`. 쿼리 순서가 결과에 영향을 주면 Id 기준 정렬 (architect.md 4장).

## 금지 (CLAUDE.md 요약)
- 상속·가상함수·과도한 추상화·God class 금지. 의미를 분명히 하는 용도로만 class 사용.
- 스마트 포인터는 꼭 필요할 때만. malloc/free/scanf 등 C 스타일 금지.
- operator는 `==` 같은 누구나 아는 것만.
- 애매하면 혼자 정하지 말고 코드에 `// QUESTION:` 주석을 남기고 보고에 적는다.

## 빌드·테스트
- 자기 하위 디렉터리의 `CMakeLists.txt`만 고친다. 최상위 `game/CMakeLists.txt`는 고치지 않는다(필요한 FetchContent는 자기 CMakeLists에서 선언).
- Rust `#[test]` 하나하나를 doctest `TEST_CASE`로 옮긴다. `tests/<crate>_test.cpp`, `add_test` 등록.
- 빌드: `cmake -S game -B game/build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build game/build && ctest --test-dir game/build --output-on-failure`. 경고 0개.
- 끝나면 자기 브랜치에 커밋한다. master에는 커밋하지 않는다.
