# Projeto Motiva – OTA (S2-CP02)

| Integrante | RM |
|---|---|
| Pedro Mirabella Tonarque | RM563419 |
| Enzo Moretto | RM563655 |
| Pedro Henrique Oliveira Marques | RM563054 |
| Lucas de Oliveira Scovini | RM563355 |
| Guilherme Mota Melo | RM565887  |
| João Ivo | RM562287 |

- Wokwi: https://wokwi.com/projects/475913625896087553
- Repositório: https://github.com/Moreettoo/repositorio-firmware

## Arquitetura

```
ESP32 (Wokwi, FW 1.0) -> Wokwi-GUEST -> GitHub (version.json + firmware_v2.bin)
consultar versão -> comparar -> baixar .bin -> atualizar -> reiniciar (FW 2.0)
```

- **FW 1.0:** 5 leituras (10–20 cm) a cada 2 s, média, sessão a cada 48 s, LED azul. Após 3 sessões verifica atualização.
- **FW 2.0:** tudo do 1.0 + ordenação, mediana e histerese (≥16 ALERTA/vermelho, ≤14 NORMAL/verde, entre 14 e 16 mantém).

## Arquivos

- `firmware_v1.ino` / `firmware_v2.ino`: códigos-fonte
- `firmware_v2.bin`: FW 2.0 compilado
- `version.json`: versão disponível e URL do `.bin`
- `wokwi/`: `diagram.json`, `libraries.txt`, `partitions.csv`

## Como executar

1. Crie um projeto ESP32 no Wokwi.
2. Cole `firmware_v1.ino` em `sketch.ino`.
3. Adicione os arquivos da pasta `wokwi/`.
4. Dê play. Após a 3ª sessão o ESP32 atualiza e reinicia como `FW 2.0 | particao app1`.
