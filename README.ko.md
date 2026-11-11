# R Weather

[English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md)

Haiku OS용 Deskbar 트레이 날씨 항목입니다. 시계 옆에 현재 날씨와 기온을 보여주고,
누르면 자세한 날씨와 짧은 예보가 나옵니다.

![Deskbar 트레이의 R Weather](docs/deskbar-tray.png)

## pkgman으로 설치

| Haiku | 저장소 추가 |
| --- | --- |
| 32비트 x86 (x86_gcc2) | `pkgman add-repo https://pkgman.rainygirl.com/x86_gcc2` |
| x86_64 | `pkgman add-repo https://pkgman.rainygirl.com/x86_64` |
| arm64 | `pkgman add-repo http://pkgman.rainygirl.com/arm64` |

```sh
pkgman install rweather
rweather            # 트레이에 추가합니다. 재부팅해도 다시 나타납니다
```

트레이에서 빼려면 `rweather --remove`를 실행하십시오.

`pkgman add-repo`가 `Operation not supported`로 실패하면 그 이미지의 네트워크 킷에
TLS가 없는 것입니다. 주소의 `https://`를 `http://`로 바꿔 쓰십시오.

## 소스에서 설치

Haiku 기기에서:

```sh
make deploy         # 빌드하고 ~/config/non-packaged/bin에 복사한 뒤 트레이에 추가합니다
```

지역은 항목 메뉴의 **Location...** 에서 바꿉니다. 날씨 데이터:
[Open-Meteo.com](https://open-meteo.com) (CC BY 4.0).

## AI 사용 고지

R Weather의 일부는 AI 코딩 도구(Anthropic Claude)의 도움을 받아 개발했습니다.
모든 코드는 작성자가 검토하고 테스트했습니다.

## 라이선스

MIT. 개발 기록은 [AGENTS.md](AGENTS.md)에 있습니다.
