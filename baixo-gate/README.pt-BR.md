# Baixo Gate (LV2)

🇺🇸 [English README](README.md)

Noise gate com histerese e filtro de chiado dinâmico, feito para o baixo no HD-II.
URI: `urn:zegkreist:lv2:baixo-gate`. Instalado em `~/.lv2/baixo-gate.lv2/`.

## Onde colocar no Carla

**Primeiro da cadeia**, antes de drive/amp (No Clip Drive, NAM). Antes do drive o
chiado está em ~-59 dBFS e a nota em -20..-1 dBFS, então o detector separa os dois
fácil. Depois do drive o chiado sobe junto e o gate começa a abrir e fechar à toa.

## Controles

| Controle | Padrão | O que faz |
|---|---|---|
| Threshold (abre) | -45 dB | Nível que abre o gate. O detector só olha 40 Hz–1,2 kHz, então o chiado agudo não abre o gate. |
| Histerese | 13 dB | Só fecha quando cai este tanto abaixo do threshold (padrão: fecha em -58). Evita o abre-e-fecha. |
| Attack | 1 ms | Rampa de abertura. Há 1 ms de lookahead, então o ataque da nota passa inteiro. |
| Hold | 100 ms | Tempo abaixo do ponto de fechamento antes de começar a fechar. |
| Release | 300 ms | Rampa de fechamento, linear em dB (soa como decaimento natural). |
| Range | -45 dB | Quanto atenua fechado. Use -90 para mudo total. |
| Filtro de chiado | 0,8 | Passa-baixa que fecha conforme a nota decai e tira o "ssss" do fim da nota sem cortar a nota. 0 = desligado. |
| Chiado: corte mínimo | 2500 Hz | Até onde o passa-baixa fecha. Mais baixo = menos chiado e mais abafado. |
| Profundidade do redutor | -30 dB | Redutor multibanda: quanto cada banda acima de 300 Hz pode ser abaixada quando só tem chiado nela. 0 = desligado. |
| Força do redutor | 4 | Mais alto tira mais chiado durante a nota, mas pode abafar o brilho da corda. |
| Redutor atuando agora (medidor) | – | Redução média das bandas agora. |
| Nivel detectado (medidor) | – | Use para calibrar: veja o valor parado (abafado) e tocando leve. |
| Reducao atual (medidor) | – | 0 = aberto, Range = fechado. |

Calibração rápida: abafe as cordas e olhe "Nivel detectado" (no HD-II deu até -56).
O ponto de fechamento (threshold − histerese) deve ficar um pouco abaixo disso e o
threshold uns 10 dB acima.

## Medições (2026-11-21, HD-II, ganho Mic 100%)

- Ruído parado: -59 dBFS RMS, chiado de banda larga. Sem hum de 60 Hz (~-92 dBFS).
- Tocando: picos até -0,8 dBFS (quase clipando; vale baixar o ganho "Mic" do HD-II).
- Resultado na gravação real: tocando = transparente; pausa -41 dB; chiado na cauda
  da nota -8 dB sem fechar o gate; silêncio puro de 25 s: gate não abriu nenhuma vez.

## Redutor de chiado tocando (multibanda)

Crossovers Linkwitz-Riley 4 em meia oitava (300, 425, 600 … 9600, 13600 Hz; 13 bandas) com
allpass de compensação; sem redução a soma é plana (`./test_gate ... sweep` dá 0,00 dB de 40 Hz a
16 kHz). Abaixo de 300 Hz nada é mexido. Em cada banda superior,
ganho = sqrt(1 − força·N/E), limitado pela redução máxima, onde N é o ruído da banda.
N começa com o valor medido no HD-II e é reaprendido sozinho sempre que o gate
está totalmente fechado (pausas).

Potência por banda é média simétrica (um seguidor de pico superestima o chiado e deixa
ele passar). Bandas >= 1,2 kHz fecham em 15 ms, as mais graves em 30 ms. Depois de um
ataque, o chiado acima de 5 kHz volta a -90 dB em ~50 ms (antes ~200 ms). O que sobra
em 2,5–5 kHz no ataque é som real da corda (17 dB acima do chiado), não ruído.

Na gravação de nota sustentada (versão de 7 bandas), o chiado acima de 5 kHz durante a nota caiu ~8 dB com
-20/2,5 e 15–28 dB com -30/5, com a faixa abaixo de 300 Hz idêntica.

## Compilar / testar / instalar

```bash
make            # compila
make test       # teste com sinal sintético
./test_gate baixo-gate.lv2/baixo_gate.so gravacoes/baixo-tocando.f32 saida.f32 [thr] [hyst]
make install    # copia para ~/.lv2 (reabra o Carla / rescan de plugins)
```

`examples/` tem trechos antes/depois em FLAC para comparar de ouvido. A pasta
`gravacoes/` (sinal cru `.f32` mono 48 kHz usado nos ajustes) fica fora do git.
