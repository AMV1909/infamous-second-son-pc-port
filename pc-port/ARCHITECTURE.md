# Decisão inicial de arquitetura

Atualização 07/10, sessão 07: [contratos recuperados](evidence/mapping/SESSAO-07.md).
Uma substituição nativa de partículas precisa preservar registros 96 B, pares
de chave/índice, argumentos de draw, processamento CPU de colisão e consumo da
metade produzida no intervalo anterior. O registry não cobre programas com ID -1
e permite remoção. O par E902349D→03E4B72E tem dependência produtora/consumidora;
ausência de packets de sincronização não é prova de independência. Instrumentação
opcional já permite validar identidades e descritores; equivalência e retirement
GPU continuam requisitos antes de integrar algoritmos nativos. Estados de boot
incompleto nas seções originais abaixo são históricos.

Atualização de desempenho (07/10, tarde): `native/reduction.cpp` e
`native/hooks.cpp` implementam uma primeira rotina reconstruída em C++, com
validação de assinatura e instalação do hook antes das threads guest. Os testes
comparam a ordem de operações com o AVX original e os estados/callbacks da
rotina. O resultado de 240 casos de paridade passou, mas o benchmark não mostrou
ganho de FPS. A versão original segue sendo o padrão.

O próximo limite relevante para migração nativa é a produção/consumo de pequenos
registros de feedback da GPU; veja `evidence/gpu-feedback-analysis.md`. A medição
por fase é inclusiva e os tempos de espera sobrepõem trabalho de outras threads.
Não é válido somar todos esses tempos ou atribuir todos os stalls a shaders.

Atualização de 07/10/2026: libc/Fios2 inicializam e o runtime apresenta frames
reais. `gpu/bridge.cpp` adapta renderer, shader recompiler, VideoOut, Gnm, AJM e
Videodec do checkout local de shadPS4 em `iss-gpu.dll`, sem seu loader/main.
A memória guest tem backing compartilhado e shadow view para acesso do renderer.
I/O de arquivos usa staging host e memcpy em modo usuário: ReadFile/WriteFile
não devem receber diretamente páginas protegidas pelo tracking gráfico.
Os serviços Windows incluem threads/sincronização, arquivos, saves, áudio SDL,
input XInput/teclado e perfil offline. A compatibilidade é incremental.
As decisões candidatas abaixo registram o planejamento original.

Alvo: runtime dedicado a inFAMOUS Second Son CUSA00004 em Windows x64, executando
código guest x86-64 original, com serviços PS4 implementados no host e tradução
gráfica para Vulkan. Não planejamos recompilar automaticamente o pseudocódigo
do Ghidra nem recuperar o projeto original do jogo.

## Papel da referência Bloodborne

Snapshot analisado:
`deadinside28/bloodborne_pc@4d4d20812c0af23a11c8b8d40f5e06a9054925ef`.

| Componente da referência | Decisão para inFAMOUS |
|---|---|
| Preparação offline SELF → imagem | adotada no primeiro protótipo; validações e identidade próprias |
| Linkagem de libc/Fios2 por identidade | implementada por identidade e versões exatas; inicialização ainda parcial |
| Runtime HLE restrito a um jogo | adotar estratégia; implementar ABI e serviços no Windows |
| NIDs com sufixos locais de Bloodborne | não usar como chave universal; interpretar tabelas por módulo |
| Renderer/shader recompiler de shadPS4 | candidato de base gráfica; respeitar GPL e atribuição |
| Decode e recording em estágios | candidato prioritário para desempenho após primeiro frame correto |
| Modelo de memória in-place experimental | investigar depois de aliasing, coerência e readbacks corretos |
| Patches de FPS, câmera, resolução, FSR/motion vectors | específicos de Bloodborne; não aplicar ao eboot de inFAMOUS |
| Build Bash/GTK/X11 e runtime POSIX | requer adaptação; nosso loader já compila em Windows com clang-cl |

O README da referência descreve o renderer atual decodificando PM4 e traduzindo
shaders GCN. A tradução direta de GnmDriver para Vulkan é citada como trabalho
futuro: não tratamos essa etapa como solução pronta.

## Ponto gráfico a investigar

O handoff registra command processor na faixa de 25–30 ms/frame com cache quente
em cenas pesadas. Isso torna preparação de draws e bindings uma área plausível
de investigação. Não estabelece uma meta atingível automaticamente.

O código da referência em
`gpu/shadps4/video_core/amdgpu/liverpool.cpp` acompanha work decodificado versus
retirado, e seus comentários descrevem falhas anteriores quando buffers guest
foram liberados cedo. O documento `docs/parallel_gpu.md` registra dependência de
estado de registradores entre command buffers e leitura de memória alterada por
DMA/WriteData. Essas restrições precisam sobreviver à adaptação.

Antes de transferir esse pipeline, vamos definir propriedade/lifetime de recursos,
snapshots imutáveis de estado, publicação de trabalho, ordem de barriers e fences
e o momento em que o guest pode reutilizar um buffer. A instrumentação deve comparar
frames e estado além de FPS. Readbacks consumidos pelo jogo não podem ser simplesmente
descartados.

## Critérios de saída por fase

| Fase | Evidência necessária |
|---|---|
| Entrada — concluída | mapa/relocations válidos, ABI testada, entrada real executada até trap esperado |
| libc/TLS — parcial | linkagem/TLS e heap direto testados; construtores param em mutexattr pendente |
| Runtime — pendente | inicialização progride com resultados/códigos de erro coerentes |
| Renderer — pendente | frame correto com dependências CPU/GPU preservadas |
| Gameplay — pendente | input/touch, saves, áudio e estado comparáveis à referência |
| 60 FPS — pendente | cenas pesadas e streaming com simulação normal, testes repetíveis |

Não há marco de boot completo, menu, gameplay ou frame gráfico aprovado neste momento.
O protótipo confirma a primeira fase e parte da segunda. A libc original já executa
seus construtores, consulta procparam, publica o heap e alcança um callback do eboot.
Consulta/alocação/mapeamento de memória direta foram implementados com uma section
Windows compartilhada. A criação de mspace nativo passa agora por
`scePthreadMutexattrInit`, o bloqueio atual. O próximo componente é atributos,
mutexes e sincronização com semântica PS4, sem assumir que uma lock recursiva
Windows substitui todos os tipos de mutex. Ciclo de vida e integração GPU do backend
de memória continuam pendentes.
