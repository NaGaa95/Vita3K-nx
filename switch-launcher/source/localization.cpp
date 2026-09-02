#include "localization.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace LauncherLocalization
{
namespace
{
constexpr std::array<Language, 9> LANGUAGE_LIST = {{
    {"system", "System"}, {"en", "English"}, {"fr", "Français"}, {"de", "Deutsch"},
    {"es", "Español"}, {"it", "Italiano"}, {"pt", "Português"},
    {"zh-Hans", "简体中文"}, {"zh-Hant", "繁體中文"},
}};

struct Entry
{
  const char* source;
  const char* fr;
  const char* de;
  const char* es;
  const char* it;
  const char* pt;
  const char* zh_Hans;
  const char* zh_Hant;
};

// Keep product names and technical terms in English when translating them would
// make the UI less precise. These strings are launcher-owned UI only; game
// names, paths, network errors and user-entered text never pass through here.
constexpr Entry ENTRIES[] = {
    {"System", "Système", "System", "Sistema", "Sistema", "Sistema", "系统", "系統"},
    {"Language", "Langue", "Sprache", "Idioma", "Lingua", "Idioma", "语言", "語言"},
    {"Launcher", "Lanceur", "Launcher", "Lanzador", "Launcher", "Launcher", "启动器", "啟動器"},
    {"Settings", "Paramètres", "Einstellungen", "Ajustes", "Impostazioni", "Definições", "设置", "設定"},
    {"Global settings", "Paramètres globaux", "Globale Einstellungen", "Ajustes globales", "Impostazioni globali", "Definições globais", "全局设置", "全域設定"},
    {"Per-game settings", "Paramètres par jeu", "Spielspezifische Einstellungen", "Ajustes por juego", "Impostazioni per gioco", "Definições por jogo", "游戏专属设置", "遊戲專屬設定"},
    {"Library & storage", "Bibliothèque et stockage", "Bibliothek & Speicher", "Biblioteca y almacenamiento", "Libreria e archiviazione", "Biblioteca e armazenamento", "游戏库与存储", "遊戲庫與儲存"},
    {"File manager", "Gestionnaire de fichiers", "Dateimanager", "Gestor de archivos", "Gestione file", "Gestor de ficheiros", "文件管理器", "檔案管理員"},
    {"Game folders", "Dossiers de jeux", "Spieleordner", "Carpetas de juegos", "Cartelle dei giochi", "Pastas de jogos", "游戏文件夹", "遊戲資料夾"},
    {"SMB network shares", "Partages réseau SMB", "SMB-Netzwerkfreigaben", "Recursos de red SMB", "Condivisioni di rete SMB", "Partilhas de rede SMB", "SMB 网络共享", "SMB 網路共用"},
    {"Theme", "Thème", "Design", "Tema", "Tema", "Tema", "主题", "主題"},
    {"Games per row", "Jeux par ligne", "Spiele pro Zeile", "Juegos por fila", "Giochi per riga", "Jogos por linha", "每行游戏数", "每行遊戲數"},
    {"Rows per page", "Lignes par page", "Zeilen pro Seite", "Filas por página", "Righe per pagina", "Linhas por página", "每页行数", "每頁行數"},
    {"Show game titles", "Afficher les titres", "Spieltitel anzeigen", "Mostrar títulos", "Mostra i titoli", "Mostrar títulos", "显示游戏标题", "顯示遊戲標題"},
    {"Show region flags", "Afficher les drapeaux de région", "Regionsflaggen anzeigen", "Mostrar banderas de región", "Mostra bandiere regionali", "Mostrar bandeiras de região", "显示区域旗帜", "顯示區域旗幟"},
    {"Show custom settings badges", "Afficher les indicateurs de paramètres personnalisés", "Markierungen für benutzerdefinierte Einstellungen anzeigen", "Mostrar indicadores de ajustes personalizados", "Mostra indicatori delle impostazioni personalizzate", "Mostrar indicadores de definições personalizadas", "显示自定义设置徽标", "顯示自訂設定徽章"},
    {"UI animations", "Animations de l'interface", "UI-Animationen", "Animaciones de la interfaz", "Animazioni dell'interfaccia", "Animações da interface", "界面动画", "介面動畫"},
    {"Sound effects", "Effets sonores", "Soundeffekte", "Efectos de sonido", "Effetti sonori", "Efeitos sonoros", "音效", "音效"},
    {"Check updates at boot", "Vérifier au démarrage", "Beim Start nach Updates suchen", "Buscar actualizaciones al iniciar", "Controlla aggiornamenti all'avvio", "Procurar atualizações ao iniciar", "启动时检查更新", "啟動時檢查更新"},
    {"Check for updates", "Rechercher des mises à jour", "Nach Updates suchen", "Buscar actualizaciones", "Controlla aggiornamenti", "Procurar atualizações", "检查更新", "檢查更新"},
    {"Check for Updates", "Rechercher des mises à jour", "Nach Updates suchen", "Buscar actualizaciones", "Controlla aggiornamenti", "Procurar atualizações", "检查更新", "檢查更新"},
    {"Download covers", "Télécharger les jaquettes", "Cover herunterladen", "Descargar carátulas", "Scarica copertine", "Transferir capas", "下载封面", "下載封面"},
    {"Cover settings", "Paramètres de la jaquette", "Cover-Einstellungen", "Ajustes de la carátula", "Impostazioni copertina", "Definições da capa", "封面设置", "封面設定"},
    {"Download from SteamGridDB", "Télécharger depuis SteamGridDB", "Von SteamGridDB herunterladen", "Descargar desde SteamGridDB", "Scarica da SteamGridDB", "Transferir do SteamGridDB", "从 SteamGridDB 下载", "從 SteamGridDB 下載"},
    {"Import cover from file", "Importer une jaquette depuis un fichier", "Cover aus Datei importieren", "Importar carátula desde un archivo", "Importa copertina da file", "Importar capa de um ficheiro", "从文件导入封面", "從檔案匯入封面"},
    {"Online artwork", "Illustration en ligne", "Online-Cover", "Ilustración en línea", "Immagine online", "Imagem online", "在线图片", "線上圖片"},
    {"Local image", "Image locale", "Lokales Bild", "Imagen local", "Immagine locale", "Imagem local", "本地图片", "本機圖片"},
    {"Search SteamGridDB and replace this game's custom cover with selected online artwork.", "Recherche sur SteamGridDB et remplace la jaquette personnalisée de ce jeu par l'illustration sélectionnée.", "Durchsucht SteamGridDB und ersetzt das benutzerdefinierte Cover dieses Spiels durch das ausgewählte Online-Bild.", "Busca en SteamGridDB y sustituye la carátula personalizada de este juego por la ilustración seleccionada.", "Cerca su SteamGridDB e sostituisce la copertina personalizzata del gioco con l'immagine selezionata.", "Pesquisa no SteamGridDB e substitui a capa personalizada deste jogo pela imagem selecionada.", "搜索 SteamGridDB，用选定的在线图片替换此游戏的自定义封面。", "搜尋 SteamGridDB，並以選定的線上圖片取代此遊戲的自訂封面。"},
    {"Choose a PNG, JPEG, WebP or BMP image from SD, USB or SMB storage. It is validated and saved safely as PNG.", "Choisissez une image PNG, JPEG, WebP ou BMP sur un stockage SD, USB ou SMB. Elle est vérifiée et enregistrée en toute sécurité au format PNG.", "Wähle ein PNG-, JPEG-, WebP- oder BMP-Bild von SD-, USB- oder SMB-Speicher. Es wird geprüft und sicher als PNG gespeichert.", "Elige una imagen PNG, JPEG, WebP o BMP del almacenamiento SD, USB o SMB. Se valida y guarda de forma segura como PNG.", "Scegli un'immagine PNG, JPEG, WebP o BMP da una memoria SD, USB o SMB. Verrà verificata e salvata in modo sicuro come PNG.", "Escolha uma imagem PNG, JPEG, WebP ou BMP do armazenamento SD, USB ou SMB. É validada e guardada em segurança como PNG.", "从 SD、USB 或 SMB 存储中选择 PNG、JPEG、WebP 或 BMP 图片。它会经过验证并安全地保存为 PNG。", "從 SD、USB 或 SMB 儲存中選擇 PNG、JPEG、WebP 或 BMP 圖片。它會經過驗證並安全地儲存為 PNG。"},
    {"Remove custom cover", "Supprimer la jaquette personnalisée", "Benutzerdefiniertes Cover entfernen", "Quitar carátula personalizada", "Rimuovi copertina personalizzata", "Remover capa personalizada", "移除自定义封面", "移除自訂封面"},
    {"Select local cover", "Sélectionner une jaquette locale", "Lokales Cover auswählen", "Seleccionar carátula local", "Seleziona copertina locale", "Selecionar capa local", "选择本地封面", "選擇本機封面"},
    {"Cover imported", "Jaquette importée", "Cover importiert", "Carátula importada", "Copertina importata", "Capa importada", "封面已导入", "封面已匯入"},
    {"Custom cover removed", "Jaquette personnalisée supprimée", "Benutzerdefiniertes Cover entfernt", "Carátula personalizada eliminada", "Copertina personalizzata rimossa", "Capa personalizada removida", "已移除自定义封面", "已移除自訂封面"},
    {"Downloading covers", "Téléchargement des jaquettes", "Cover werden heruntergeladen", "Descargando carátulas", "Download delle copertine", "A transferir capas", "正在下载封面", "正在下載封面"},
    {"Download cover", "Télécharger la jaquette", "Cover herunterladen", "Descargar carátula", "Scarica copertina", "Transferir capa", "下载封面", "下載封面"},
    {"Search", "Rechercher", "Suchen", "Buscar", "Cerca", "Pesquisar", "搜索", "搜尋"},
    {"Search games", "Rechercher des jeux", "Spiele suchen", "Buscar juegos", "Cerca giochi", "Pesquisar jogos", "搜索游戏", "搜尋遊戲"},
    {"Favorites", "Favoris", "Favoriten", "Favoritos", "Preferiti", "Favoritos", "收藏", "收藏"},
    {"Collections", "Collections", "Sammlungen", "Colecciones", "Raccolte", "Coleções", "合集", "合集"},
    {"Manage collections", "Gérer les collections", "Sammlungen verwalten", "Gestionar colecciones", "Gestisci raccolte", "Gerir coleções", "管理合集", "管理合集"},
    {"All games", "Tous les jeux", "Alle Spiele", "Todos los juegos", "Tutti i giochi", "Todos os jogos", "所有游戏", "所有遊戲"},
    {"Add to favorites", "Ajouter aux favoris", "Zu Favoriten hinzufügen", "Añadir a favoritos", "Aggiungi ai preferiti", "Adicionar aos favoritos", "添加到收藏", "加入收藏"},
    {"Remove from favorites", "Retirer des favoris", "Aus Favoriten entfernen", "Quitar de favoritos", "Rimuovi dai preferiti", "Remover dos favoritos", "从收藏中移除", "從收藏中移除"},
    {"Create collection", "Créer une collection", "Sammlung erstellen", "Crear colección", "Crea raccolta", "Criar coleção", "创建合集", "建立合集"},
    {"Rename collection", "Renommer la collection", "Sammlung umbenennen", "Renombrar colección", "Rinomina raccolta", "Renomear coleção", "重命名合集", "重新命名合集"},
    {"Delete collection", "Supprimer la collection", "Sammlung löschen", "Eliminar colección", "Elimina raccolta", "Eliminar coleção", "删除合集", "刪除合集"},
    {"No games match this view", "Aucun jeu ne correspond à cette vue", "Keine Spiele entsprechen dieser Ansicht", "Ningún juego coincide con esta vista", "Nessun gioco corrisponde a questa vista", "Nenhum jogo corresponde a esta vista", "没有游戏符合此视图", "沒有遊戲符合此檢視"},
    {"NO COVER", "AUCUNE JAQUETTE", "KEIN COVER", "SIN CARÁTULA", "NESSUNA COPERTINA", "SEM CAPA", "无封面", "無封面"},
    {"Loading game library...", "Chargement de la bibliothèque...", "Spielebibliothek wird geladen...", "Cargando la biblioteca...", "Caricamento della libreria...", "A carregar a biblioteca...", "正在加载游戏库...", "正在載入遊戲庫..."},
    {"The first page will appear as soon as it is ready.", "La première page s'affichera dès qu'elle sera prête.", "Die erste Seite erscheint, sobald sie bereit ist.", "La primera página aparecerá en cuanto esté lista.", "La prima pagina apparirà appena pronta.", "A primeira página aparecerá assim que estiver pronta.", "第一页就绪后便会显示。", "第一頁就緒後便會顯示。"},
    {"Scanning game library...", "Analyse de la bibliothèque...", "Spielebibliothek wird durchsucht...", "Analizando la biblioteca...", "Scansione della libreria...", "A analisar a biblioteca...", "正在扫描游戏库...", "正在掃描遊戲庫..."},
    {"Reset", "Réinitialiser", "Zurücksetzen", "Restablecer", "Ripristina", "Repor", "重置", "重設"},
    {"Setting reset to default", "Paramètre réinitialisé à sa valeur par défaut", "Einstellung auf Standardwert zurückgesetzt", "Ajuste restablecido al valor predeterminado", "Impostazione ripristinata al valore predefinito", "Definição reposta para o valor predefinido", "设置已重置为默认值", "設定已重設為預設值"},
    {"Use global", "Utiliser le réglage global", "Globale Einstellung verwenden", "Usar ajuste global", "Usa impostazione globale", "Usar definição global", "使用全局", "使用全域"},
    {"Current:", "Actuel :", "Aktuell:", "Actual:", "Attuale:", "Atual:", "当前：", "目前："},
    {"Close", "Fermer", "Schließen", "Cerrar", "Chiudi", "Fechar", "关闭", "關閉"},
    {"Choose", "Choisir", "Auswählen", "Elegir", "Scegli", "Escolher", "选择", "選擇"},
    {"Change", "Modifier", "Ändern", "Cambiar", "Modifica", "Alterar", "更改", "變更"},
    {"Back", "Retour", "Zurück", "Atrás", "Indietro", "Voltar", "返回", "返回"},
    {"Yes", "Oui", "Ja", "Sí", "Sì", "Sim", "是", "是"},
    {"No", "Non", "Nein", "No", "No", "Não", "否", "否"},
    {"Exit", "Quitter", "Beenden", "Salir", "Esci", "Sair", "退出", "退出"},
    {"Exit Vita3K?", "Quitter Vita3K ?", "Vita3K beenden?", "¿Salir de Vita3K?", "Uscire da Vita3K?", "Sair do Vita3K?", "退出 Vita3K？", "退出 Vita3K？"},
    {"Return to the HOME Menu?", "Retourner au menu HOME ?", "Zum HOME-Menü zurückkehren?", "¿Volver al menú HOME?", "Tornare al menu HOME?", "Voltar ao Menu HOME?", "返回 HOME 菜单？", "返回 HOME 選單？"},
    {"Active background operations will be cancelled safely.", "Les opérations en arrière-plan seront annulées en toute sécurité.", "Aktive Hintergrundvorgänge werden sicher abgebrochen.", "Las operaciones en segundo plano se cancelarán de forma segura.", "Le operazioni in background verranno annullate in sicurezza.", "As operações em segundo plano serão canceladas em segurança.", "正在进行的后台操作将被安全取消。", "進行中的背景操作將被安全取消。"},
    {"Closing Vita3K...", "Fermeture de Vita3K...", "Vita3K wird beendet...", "Cerrando Vita3K...", "Chiusura di Vita3K...", "A fechar o Vita3K...", "正在关闭 Vita3K...", "正在關閉 Vita3K..."},
    {"Finishing background operations safely.", "Finalisation sécurisée des opérations en arrière-plan.", "Hintergrundvorgänge werden sicher abgeschlossen.", "Finalizando de forma segura las operaciones en segundo plano.", "Completamento sicuro delle operazioni in background.", "A concluir as operações em segundo plano com segurança.", "正在安全地完成后台操作。", "正在安全地完成背景操作。"},
    {"Applet mode installer", "Installation en mode applet", "Applet-Modus-Installer", "Instalador del modo applet", "Installazione in modalità applet", "Instalador do modo applet", "Applet 模式安装程序", "Applet 模式安裝程式"},
    {"Vita3K is running in applet mode.", "Vita3K fonctionne en mode applet.", "Vita3K läuft im Applet-Modus.", "Vita3K se está ejecutando en modo applet.", "Vita3K è in esecuzione in modalità applet.", "O Vita3K está a ser executado no modo applet.", "Vita3K 正在以 applet 模式运行。", "Vita3K 正在以 applet 模式執行。"},
    {"Applet mode has limited memory and is not suitable for emulation.", "Le mode applet dispose de peu de mémoire et ne convient pas à l'émulation.", "Der Applet-Modus hat wenig Speicher und eignet sich nicht zur Emulation.", "El modo applet tiene memoria limitada y no es adecuado para emular.", "La modalità applet ha memoria limitata e non è adatta all'emulazione.", "O modo applet tem memória limitada e não é adequado à emulação.", "Applet 模式内存有限，不适合模拟。", "Applet 模式的記憶體有限，不適合模擬。"},
    {"Install a HOME Menu shortcut to use full memory and normal performance.", "Installez un raccourci dans le menu HOME pour utiliser toute la mémoire et les performances normales.", "Installiere eine HOME-Menü-Verknüpfung für vollen Speicher und normale Leistung.", "Instala un acceso directo en el menú HOME para usar toda la memoria y el rendimiento normal.", "Installa un collegamento nel menu HOME per usare tutta la memoria e le prestazioni normali.", "Instale um atalho no Menu HOME para usar toda a memória e o desempenho normal.", "安装主菜单快捷方式以使用完整内存和正常性能。", "安裝主選單捷徑以使用完整記憶體和正常效能。"},
    {"Install to HOME Menu", "Installer dans le menu HOME", "Im HOME-Menü installieren", "Instalar en el menú HOME", "Installa nel menu HOME", "Instalar no Menu HOME", "安装到主菜单", "安裝到主選單"},
    {"Installing HOME Menu shortcut...", "Installation du raccourci du menu HOME...", "HOME-Menü-Verknüpfung wird installiert...", "Instalando el acceso directo del menú HOME...", "Installazione del collegamento nel menu HOME...", "A instalar o atalho do Menu HOME...", "正在安装主菜单快捷方式...", "正在安裝主選單捷徑..."},
    {"Preparing Vita3K...", "Préparation de Vita3K...", "Vita3K wird vorbereitet...", "Preparando Vita3K...", "Preparazione di Vita3K...", "A preparar o Vita3K...", "正在准备 Vita3K...", "正在準備 Vita3K..."},
    {"Installed on the HOME Menu.", "Installé dans le menu HOME.", "Im HOME-Menü installiert.", "Instalado en el menú HOME.", "Installato nel menu HOME.", "Instalado no Menu HOME.", "已安装到主菜单。", "已安裝到主選單。"},
    {"Installation failed", "Échec de l'installation", "Installation fehlgeschlagen", "Error de instalación", "Installazione non riuscita", "Falha na instalação", "安装失败", "安裝失敗"},
    {"Try again", "Réessayer", "Erneut versuchen", "Reintentar", "Riprova", "Tentar novamente", "重试", "重試"},
    {"Install", "Installer", "Installieren", "Instalar", "Installa", "Instalar", "安装", "安裝"},
    {"Off", "Désactivé", "Aus", "Desactivado", "Disattivato", "Desativado", "关闭", "關閉"},
    {"On", "Activé", "Ein", "Activado", "Attivato", "Ativado", "开启", "開啟"},
    {"Automatic", "Automatique", "Automatisch", "Automático", "Automatico", "Automático", "自动", "自動"},
    {"Manual", "Manuel", "Manuell", "Manual", "Manuale", "Manual", "手动", "手動"},
    {"Disabled", "Désactivé", "Deaktiviert", "Desactivado", "Disabilitato", "Desativado", "已禁用", "已停用"},
    {"Compatibility", "Compatibilité", "Kompatibilität", "Compatibilidad", "Compatibilità", "Compatibilidade", "兼容性", "相容性"},
    {"Performance", "Performances", "Leistung", "Rendimiento", "Prestazioni", "Desempenho", "性能", "效能"},
    {"Graphics", "Graphismes", "Grafik", "Gráficos", "Grafica", "Gráficos", "图形", "圖形"},
    {"Display backend", "Moteur d'affichage", "Anzeige-Backend", "Motor de pantalla", "Backend di rendering", "Motor de apresentação", "显示后端", "顯示後端"},
    {"Audio", "Audio", "Audio", "Audio", "Audio", "Áudio", "音频", "音訊"},
    {"Controls", "Commandes", "Steuerung", "Controles", "Controlli", "Controlos", "操控", "操控"},
    {"Diagnostics", "Diagnostic", "Diagnose", "Diagnóstico", "Diagnostica", "Diagnóstico", "诊断", "診斷"},
    {"Network", "Réseau", "Netzwerk", "Red", "Rete", "Rede", "网络", "網路"},
    {"Interface", "Interface", "Oberfläche", "Interfaz", "Interfaccia", "Interface", "界面", "介面"},
    {"Modding", "Modding", "Modding", "Modding", "Modding", "Modding", "模组", "模組"},
    {"Vita system", "Système Vita", "Vita-System", "Sistema Vita", "Sistema Vita", "Sistema Vita", "Vita 系统", "Vita 系統"},
    {"System firmware", "Micrologiciel système", "System-Firmware", "Firmware del sistema", "Firmware di sistema", "Firmware do sistema", "系统固件", "系統韌體"},
    {"Library layout", "Disposition de la bibliothèque", "Bibliothekslayout", "Diseño de la biblioteca", "Layout della libreria", "Disposição da biblioteca", "游戏库布局", "遊戲庫佈局"},
    {"Launcher appearance", "Apparence du lanceur", "Launcher-Darstellung", "Apariencia del lanzador", "Aspetto del launcher", "Aspeto do launcher", "启动器外观", "啟動器外觀"},
    {"Launcher audio", "Audio du lanceur", "Launcher-Audio", "Audio del lanzador", "Audio del launcher", "Áudio do launcher", "启动器音频", "啟動器音訊"},
    {"Launcher language", "Langue du lanceur", "Launcher-Sprache", "Idioma del lanzador", "Lingua del launcher", "Idioma do launcher", "启动器语言", "啟動器語言"},
    {"Launcher updates", "Mises à jour du lanceur", "Launcher-Aktualisierungen", "Actualizaciones del lanzador", "Aggiornamenti del launcher", "Atualizações do launcher", "启动器更新", "啟動器更新"},
    {"Artwork service", "Service de jaquettes", "Cover-Dienst", "Servicio de carátulas", "Servizio copertine", "Serviço de capas", "封面图服务", "封面圖服務"},
    {"Frame generation", "Génération d'images", "Frame-Generierung", "Generación de fotogramas", "Generazione fotogrammi", "Geração de fotogramas", "帧生成", "幀生成"},
    {"Frame generation quality", "Qualité de génération d'images", "Qualität der Frame-Generierung", "Calidad de generación de fotogramas", "Qualità della generazione fotogrammi", "Qualidade da geração de fotogramas", "帧生成质量", "幀生成品質"},
    {"Frame generation performance", "Performances de génération d'images", "Leistung der Frame-Generierung", "Rendimiento de generación de fotogramas", "Prestazioni della generazione fotogrammi", "Desempenho da geração de fotogramas", "帧生成性能", "幀生成效能"},
    {"Controls whether Vita system modules are loaded automatically or from a manual list. Automatic is recommended for most games.", "Détermine si les modules système Vita sont chargés automatiquement ou depuis une liste manuelle. Le mode automatique est recommandé pour la plupart des jeux.", "Legt fest, ob Vita-Systemmodule automatisch oder aus einer manuellen Liste geladen werden. Automatisch wird für die meisten Spiele empfohlen.", "Define si los módulos del sistema Vita se cargan automáticamente o desde una lista manual. Se recomienda Automático para la mayoría de juegos.", "Stabilisce se i moduli di sistema Vita vengono caricati automaticamente o da un elenco manuale. Automatico è consigliato per la maggior parte dei giochi.", "Define se os módulos do sistema Vita são carregados automaticamente ou através de uma lista manual. Automático é recomendado para a maioria dos jogos.", "控制 Vita 系统模块是自动加载还是从手动列表加载。大多数游戏建议使用「自动」。", "控制 Vita 系統模組是自動載入還是從手動清單載入。大多數遊戲建議使用「自動」。"},
    {"Enables Vita3K CPU optimisations. Disable only when diagnosing a title-specific CPU emulation problem.", "Active les optimisations CPU de Vita3K. Désactivez-les uniquement pour diagnostiquer un problème d'émulation CPU propre à un jeu.", "Aktiviert Vita3K-CPU-Optimierungen. Nur zur Diagnose eines spielspezifischen CPU-Emulationsproblems deaktivieren.", "Activa las optimizaciones de CPU de Vita3K. Desactívalas solo al diagnosticar un problema de emulación de CPU específico de un juego.", "Abilita le ottimizzazioni CPU di Vita3K. Disabilitale solo per diagnosticare un problema di emulazione CPU specifico di un gioco.", "Ativa as otimizações de CPU do Vita3K. Desative apenas para diagnosticar um problema de emulação de CPU específico de um jogo.", "启用 Vita3K CPU 优化。仅在诊断特定游戏的 CPU 模拟问题时才禁用。", "啟用 Vita3K CPU 最佳化。僅在診斷特定遊戲的 CPU 模擬問題時才停用。"},
    {"Prepares Vulkan LSFG 2x support for this game. Open the in-game quick menu with L + R + Plus to turn generated frames on or off. It does not increase emulation speed.", "Prépare la prise en charge de Vulkan LSFG 2x pour ce jeu. Ouvrez le menu rapide en jeu avec L + R + Plus pour activer ou désactiver les images générées. Cela n'accélère pas l'émulation.", "Bereitet Vulkan-LSFG-2x für dieses Spiel vor. Im Spiel mit L + R + Plus das Schnellmenü öffnen, um generierte Frames ein- oder auszuschalten. Die Emulationsgeschwindigkeit steigt dadurch nicht.", "Prepara la compatibilidad con Vulkan LSFG 2x para este juego. Abre el menú rápido con L + R + Plus para activar o desactivar los fotogramas generados. No aumenta la velocidad de emulación.", "Prepara il supporto Vulkan LSFG 2x per questo gioco. Apri il menu rapido in gioco con L + R + Plus per attivare o disattivare i fotogrammi generati. Non aumenta la velocità di emulazione.", "Prepara o suporte Vulkan LSFG 2x para este jogo. Abra o menu rápido no jogo com L + R + Plus para ativar ou desativar os fotogramas gerados. Não aumenta a velocidade da emulação.", "为这款游戏准备 Vulkan LSFG 2x 支持。按 L + R + 加号打开游戏内快捷菜单，开启或关闭生成的帧。它不会提高模拟速度。", "為這款遊戲準備 Vulkan LSFG 2x 支援。按 L + R + 加號開啟遊戲內快速選單，開啟或關閉生成的幀。它不會提高模擬速度。"},
    {"Sets the optical-flow resolution. Quarter is recommended on Switch; Half can improve motion detail but costs more GPU time and memory.", "Règle la résolution du flux optique. Quarter est recommandé sur Switch ; Half peut améliorer les détails en mouvement, mais utilise plus de temps GPU et de mémoire.", "Legt die Auflösung des optischen Flusses fest. Quarter wird auf Switch empfohlen; Half kann Bewegungsdetails verbessern, benötigt aber mehr GPU-Zeit und Speicher.", "Ajusta la resolución del flujo óptico. Se recomienda Quarter en Switch; Half puede mejorar el detalle en movimiento, pero consume más GPU y memoria.", "Imposta la risoluzione del flusso ottico. Quarter è consigliato su Switch; Half può migliorare i dettagli in movimento ma richiede più GPU e memoria.", "Define a resolução do fluxo ótico. Quarter é recomendado na Switch; Half pode melhorar o detalhe em movimento, mas usa mais GPU e memória.", "设置光流分辨率。Switch 上建议使用 Quarter（四分之一）；Half（二分之一）可以改善运动细节，但会消耗更多 GPU 时间和内存。", "設定光流解析度。Switch 上建議使用 Quarter（四分之一）；Half（二分之一）可以改善運動細節，但會消耗更多 GPU 時間和記憶體。"},
    {"Uses LSFG's lighter performance-oriented path. Disable it only when image quality matters more than GPU headroom.", "Utilise le chemin LSFG allégé axé sur les performances. Désactivez-le uniquement si la qualité d'image est prioritaire sur la marge GPU.", "Nutzt den leichteren, leistungsorientierten LSFG-Pfad. Nur deaktivieren, wenn Bildqualität wichtiger als GPU-Reserve ist.", "Usa la ruta ligera de LSFG orientada al rendimiento. Desactívala solo si la calidad de imagen importa más que el margen de GPU.", "Usa il percorso LSFG più leggero orientato alle prestazioni. Disabilitalo solo se la qualità dell'immagine conta più del margine GPU.", "Usa o caminho LSFG mais leve, orientado ao desempenho. Desative apenas se a qualidade de imagem for mais importante do que a margem da GPU.", "使用 LSFG 更轻量、面向性能的路径。仅在图像质量比 GPU 余量更重要时才禁用。", "使用 LSFG 更輕量、以效能為導向的途徑。僅在影像品質比 GPU 餘量更重要時才停用。"},
    {"Chooses the Switch renderer. Vulkan (NVK) is recommended and supports LSFG. OpenGL uses native NVC0, while Zink runs OpenGL on NVK as an additional compatibility path.", "Choisit le moteur de rendu de la Switch. Vulkan (NVK) est recommandé et prend en charge LSFG. OpenGL utilise le pilote NVC0 natif, tandis que Zink exécute OpenGL sur NVK comme voie de compatibilité supplémentaire.", "Wählt den Switch-Renderer. Vulkan (NVK) wird empfohlen und unterstützt LSFG. OpenGL verwendet das native NVC0, während Zink OpenGL über NVK als zusätzlichen Kompatibilitätspfad ausführt.", "Elige el renderizador de Switch. Vulkan (NVK) es el recomendado y admite LSFG. OpenGL usa NVC0 nativo, mientras Zink ejecuta OpenGL sobre NVK como ruta de compatibilidad adicional.", "Sceglie il renderer di Switch. Vulkan (NVK) è consigliato e supporta LSFG. OpenGL usa NVC0 nativo, mentre Zink esegue OpenGL su NVK come percorso di compatibilità aggiuntivo.", "Seleciona o renderizador da Switch. Vulkan (NVK) é recomendado e suporta LSFG. OpenGL usa NVC0 nativo, enquanto o Zink executa OpenGL sobre NVK como via de compatibilidade adicional.", "选择 Switch 渲染器。推荐 Vulkan (NVK)，支持 LSFG。OpenGL 使用原生 NVC0，而 Zink 在 NVK 上运行 OpenGL，作为额外的兼容路径。", "選擇 Switch 渲染器。建議使用 Vulkan (NVK)，支援 LSFG。OpenGL 使用原生 NVC0，而 Zink 在 NVK 上執行 OpenGL，作為額外的相容途徑。"},
    {"Controls renderer selection, graphics quality, scaling, caches, and performance diagnostics.", "Contrôle le choix du moteur de rendu, la qualité graphique, la mise à l'échelle, les caches et les diagnostics de performances.", "Steuert Renderer-Auswahl, Grafikqualität, Skalierung, Caches und Leistungsdiagnose.", "Controla la selección del renderizador, la calidad gráfica, el escalado, las cachés y los diagnósticos de rendimiento.", "Controlla la scelta del renderer, la qualità grafica, il ridimensionamento, le cache e la diagnostica delle prestazioni.", "Controla a seleção do renderizador, a qualidade gráfica, a escala, as caches e os diagnósticos de desempenho.", "控制渲染器选择、图形质量、缩放、缓存和性能诊断。", "控制渲染器選擇、圖形品質、縮放、快取和效能診斷。"},
    {"Scales the Vita render resolution. Higher values sharpen the image but increase GPU and memory cost.", "Met à l'échelle la résolution de rendu Vita. Une valeur élevée affine l'image, mais augmente le coût GPU et mémoire.", "Skaliert die Vita-Renderauflösung. Höhere Werte schärfen das Bild, erhöhen aber GPU- und Speicherbedarf.", "Escala la resolución de renderizado de Vita. Valores altos hacen la imagen más nítida, pero aumentan el uso de GPU y memoria.", "Scala la risoluzione di rendering Vita. Valori più alti rendono l'immagine più nitida, ma aumentano il costo GPU e memoria.", "Dimensiona a resolução de renderização Vita. Valores maiores tornam a imagem mais nítida, mas aumentam o uso de GPU e memória.", "缩放 Vita 渲染分辨率。更高的值使图像更清晰，但会增加 GPU 和内存开销。", "縮放 Vita 渲染解析度。較高的值使影像更清晰，但會增加 GPU 和記憶體開銷。"},
    {"Selects how Vita GPU memory is mirrored for Vulkan. Double buffer is the Switch default and the only mapped mode this GPU handles well. Disabled turns mapping off entirely, which some games need.", "Choisit comment la mémoire GPU Vita est répliquée pour Vulkan. Double buffer est le réglage par défaut sur Switch et le seul mode mappé que ce GPU gère bien. Désactivé coupe entièrement le mappage, ce dont certains jeux ont besoin.", "Wählt, wie Vita-GPU-Speicher für Vulkan gespiegelt wird. Double buffer ist der Switch-Standard und der einzige gemappte Modus, den diese GPU gut beherrscht. Deaktiviert schaltet das Mapping ganz ab, was manche Spiele benötigen.", "Elige cómo se refleja la memoria GPU de Vita para Vulkan. Double buffer es el valor predeterminado en Switch y el único modo mapeado que esta GPU maneja bien. Desactivado desactiva el mapeo por completo, algo que algunos juegos necesitan.", "Sceglie come viene replicata la memoria GPU Vita per Vulkan. Double buffer è il valore predefinito su Switch e l’unica modalità mappata che questa GPU gestisce bene. Disabilitato disattiva del tutto la mappatura, cosa che alcuni giochi richiedono.", "Seleciona como a memória GPU da Vita é espelhada para Vulkan. Double buffer é o padrão na Switch e o único modo mapeado que esta GPU lida bem. Desativado desliga o mapeamento por completo, o que alguns jogos precisam.", "选择 Vita GPU 内存如何为 Vulkan 镜像。双缓冲是 Switch 的默认设置，也是该 GPU 能良好处理的唯一映射模式。禁用会完全关闭映射，某些游戏需要这样做。", "選擇 Vita GPU 記憶體如何為 Vulkan 鏡像。雙緩衝是 Switch 的預設設定，也是此 GPU 能良好處理的唯一映射模式。停用會完全關閉映射，某些遊戲需要這樣做。"},
    {"Uses more accurate GPU behavior for games that render incorrectly, at a possible performance cost.", "Utilise un comportement GPU plus précis pour les jeux dont le rendu est incorrect, avec un coût possible en performances.", "Nutzt genaueres GPU-Verhalten für Spiele mit fehlerhafter Darstellung, möglicherweise auf Kosten der Leistung.", "Usa un comportamiento de GPU más preciso para juegos con renderizado incorrecto, con posible coste de rendimiento.", "Usa un comportamento GPU più accurato per i giochi con rendering errato, con un possibile costo in prestazioni.", "Usa um comportamento de GPU mais preciso para jogos com renderização incorreta, com possível custo de desempenho.", "对渲染不正确的游戏使用更精确的 GPU 行为，可能以性能为代价。", "對渲染不正確的遊戲使用更精確的 GPU 行為，可能以效能為代價。"},
    {"Chooses the final image scaling filter. Nearest is sharp, Bilinear is inexpensive, and advanced filters cost more GPU time.", "Choisit le filtre final de mise à l'échelle. Nearest est net, Bilinear est peu coûteux et les filtres avancés utilisent plus de GPU.", "Wählt den finalen Bildskalierungsfilter. Nearest ist scharf, Bilinear ist günstig und erweiterte Filter benötigen mehr GPU-Zeit.", "Elige el filtro final de escalado. Nearest es nítido, Bilinear es ligero y los filtros avanzados consumen más GPU.", "Sceglie il filtro finale di ridimensionamento. Nearest è nitido, Bilinear è leggero e i filtri avanzati richiedono più GPU.", "Seleciona o filtro final de escala. Nearest é nítido, Bilinear é leve e os filtros avançados usam mais GPU.", "选择最终的图像缩放滤镜。Nearest 锐利，Bilinear 开销低，高级滤镜消耗更多 GPU 时间。", "選擇最終的影像縮放濾鏡。Nearest 銳利，Bilinear 開銷低，進階濾鏡消耗更多 GPU 時間。"},
    {"Synchronizes presentation to the display refresh to avoid visible tearing. On drivers that expose only FIFO presentation this remains enabled by the driver.", "Synchronise l'affichage avec le rafraîchissement de l'écran pour éviter le tearing. Sur les pilotes proposant uniquement FIFO, le pilote le maintient activé.", "Synchronisiert die Ausgabe mit der Bildwiederholrate, um Tearing zu vermeiden. Bei Treibern mit ausschließlich FIFO bleibt dies durch den Treiber aktiviert.", "Sincroniza la presentación con el refresco de pantalla para evitar tearing. En controladores que solo ofrecen FIFO, el controlador lo mantiene activado.", "Sincronizza la presentazione con il refresh dello schermo per evitare tearing. Sui driver che offrono solo FIFO resta attivo tramite il driver.", "Sincroniza a apresentação com a atualização do ecrã para evitar tearing. Em drivers que apenas expõem FIFO, permanece ativado pelo driver.", "将画面同步到显示器刷新率以避免可见的撕裂。在仅暴露 FIFO 呈现的驱动上，该选项由驱动保持启用。", "將畫面同步到顯示器更新率以避免可見的撕裂。在僅暴露 FIFO 呈現的驅動程式上，此選項由驅動程式保持啟用。"},
    {"Improves texture clarity at oblique angles. Higher levels use additional GPU bandwidth.", "Améliore la netteté des textures sous des angles obliques. Les niveaux élevés utilisent davantage de bande passante GPU.", "Verbessert die Texturschärfe in schrägen Winkeln. Höhere Stufen benötigen zusätzliche GPU-Bandbreite.", "Mejora la claridad de las texturas en ángulos oblicuos. Los niveles altos usan más ancho de banda de GPU.", "Migliora la nitidezza delle texture ad angoli obliqui. Livelli più alti usano più banda GPU.", "Melhora a nitidez das texturas em ângulos oblíquos. Níveis maiores usam mais largura de banda da GPU.", "改善斜角下的纹理清晰度。更高等级会使用额外的 GPU 带宽。", "改善斜角下的紋理清晰度。更高等級會使用額外的 GPU 頻寬。"},
    {"Skips expensive surface synchronization. The Switch default favors performance, but a game with missing or stale graphics may need it enabled.", "Ignore la synchronisation coûteuse des surfaces. Le réglage Switch privilégie les performances, mais un jeu avec des graphismes absents ou figés peut nécessiter son activation.", "Überspringt aufwendige Oberflächensynchronisierung. Der Switch-Standard bevorzugt Leistung; bei fehlender oder veralteter Grafik kann sie nötig sein.", "Omite la costosa sincronización de superficies. El valor de Switch favorece el rendimiento, pero un juego con gráficos ausentes o desactualizados puede necesitarla.", "Salta la costosa sincronizzazione delle superfici. Il valore Switch favorisce le prestazioni, ma un gioco con grafica mancante o non aggiornata può richiederla.", "Ignora a sincronização dispendiosa de superfícies. O padrão da Switch favorece o desempenho, mas um jogo com gráficos ausentes ou desatualizados pode precisar dela.", "跳过昂贵的表面同步。Switch 默认值偏向性能，但图形缺失或过时的游戏可能需要启用它。", "跳過昂貴的表面同步。Switch 預設值偏向效能，但圖形缺失或過時的遊戲可能需要啟用它。"},
    {"Caches decoded and uploaded textures. Disabling is intended only for graphics troubleshooting.", "Met en cache les textures décodées et envoyées. La désactivation sert uniquement au diagnostic graphique.", "Speichert dekodierte und hochgeladene Texturen zwischen. Deaktivieren ist nur zur Grafikdiagnose gedacht.", "Guarda en caché las texturas decodificadas y subidas. Desactívalo solo para diagnosticar problemas gráficos.", "Memorizza le texture decodificate e caricate. La disattivazione serve solo alla diagnostica grafica.", "Coloca em cache as texturas descodificadas e enviadas. Desative apenas para diagnóstico gráfico.", "缓存已解码和已上传的纹理。禁用仅用于图形故障排查。", "快取已解碼和已上傳的紋理。停用僅用於圖形疑難排解。"},
    {"Compiles Vulkan pipelines asynchronously to reduce stalls. Newly encountered effects can appear briefly after compilation.", "Compile les pipelines Vulkan de façon asynchrone pour réduire les blocages. Les nouveaux effets peuvent apparaître brièvement après leur compilation.", "Kompiliert Vulkan-Pipelines asynchron, um Stocken zu verringern. Neue Effekte können kurz verzögert erscheinen.", "Compila las canalizaciones Vulkan de forma asíncrona para reducir pausas. Los efectos nuevos pueden aparecer brevemente tras compilarse.", "Compila le pipeline Vulkan in modo asincrono per ridurre i blocchi. Gli effetti nuovi possono apparire con un breve ritardo.", "Compila pipelines Vulkan de forma assíncrona para reduzir pausas. Efeitos novos podem aparecer com um pequeno atraso.", "异步编译 Vulkan 管线以减少卡顿。新遇到的特效在编译后可能短暂出现。", "非同步編譯 Vulkan 管線以減少停頓。新遇到的特效在編譯後可能短暫出現。"},
    {"Shows Vita3K's shader compilation indicator while new graphics pipelines are prepared.", "Affiche l'indicateur de compilation des shaders de Vita3K pendant la préparation de nouveaux pipelines graphiques.", "Zeigt Vita3Ks Shader-Kompilierungsanzeige, während neue Grafik-Pipelines vorbereitet werden.", "Muestra el indicador de compilación de shaders de Vita3K mientras se preparan nuevas canalizaciones gráficas.", "Mostra l'indicatore di compilazione shader di Vita3K durante la preparazione di nuove pipeline grafiche.", "Mostra o indicador de compilação de shaders do Vita3K enquanto novas pipelines gráficas são preparadas.", "在准备新的图形管线时显示 Vita3K 的着色器编译指示器。", "在準備新的圖形管線時顯示 Vita3K 的著色器編譯指示器。"},
    {"Reuses compiled shaders between sessions to reduce later stutter. Clearing a broken cache is available from the game menu.", "Réutilise les shaders compilés entre les sessions pour réduire les saccades. Un cache défectueux peut être effacé depuis le menu du jeu.", "Verwendet kompilierte Shader sitzungsübergreifend, um späteres Ruckeln zu verringern. Ein defekter Cache kann im Spielmenü gelöscht werden.", "Reutiliza shaders compilados entre sesiones para reducir tirones posteriores. Se puede borrar una caché dañada desde el menú del juego.", "Riutilizza gli shader compilati tra le sessioni per ridurre gli scatti. Una cache danneggiata può essere cancellata dal menu del gioco.", "Reutiliza shaders compilados entre sessões para reduzir engasgos posteriores. Uma cache danificada pode ser limpa no menu do jogo.", "在会话间复用已编译的着色器以减少后续卡顿。可在游戏菜单中清除损坏的缓存。", "在會話間重複使用已編譯的著色器以減少後續卡頓。可在遊戲選單中清除損壞的快取。"},
    {"Loads replacement textures from Vita3K's texture import directory.", "Charge les textures de remplacement depuis le dossier d'importation de Vita3K.", "Lädt Ersatztexturen aus Vita3Ks Textur-Importordner.", "Carga texturas de reemplazo desde la carpeta de importación de Vita3K.", "Carica texture sostitutive dalla cartella di importazione di Vita3K.", "Carrega texturas de substituição a partir da pasta de importação do Vita3K.", "从 Vita3K 的纹理导入目录加载替换纹理。", "從 Vita3K 的紋理匯入目錄載入替換紋理。"},
    {"Dumps textures used by the game for replacement or inspection. This increases SD-card I/O.", "Exporte les textures utilisées par le jeu pour les remplacer ou les inspecter. Cela augmente les accès à la carte SD.", "Speichert vom Spiel verwendete Texturen zum Ersetzen oder Prüfen. Dies erhöht die SD-Karten-I/O.", "Vuelca las texturas usadas por el juego para reemplazarlas o revisarlas. Esto aumenta la E/S de la tarjeta SD.", "Esporta le texture usate dal gioco per sostituzione o analisi. Aumenta l'I/O della scheda SD.", "Exporta as texturas usadas pelo jogo para substituição ou inspeção. Isto aumenta a E/S do cartão SD.", "导出游戏使用的纹理以供替换或检查。这会增加 SD 卡 I/O。", "匯出遊戲使用的紋理以供替換或檢查。這會增加 SD 卡 I/O。"},
    {"Writes exported textures as PNG rather than their raw format. PNG is convenient but slower to encode.", "Écrit les textures exportées en PNG plutôt qu'au format brut. Le PNG est pratique, mais plus lent à encoder.", "Schreibt exportierte Texturen als PNG statt im Rohformat. PNG ist praktisch, aber langsamer zu kodieren.", "Guarda las texturas exportadas como PNG en vez de formato sin procesar. PNG es práctico, pero tarda más en codificarse.", "Salva le texture esportate come PNG invece che in formato grezzo. PNG è comodo ma più lento da codificare.", "Guarda as texturas exportadas como PNG em vez do formato bruto. PNG é prático, mas mais lento a codificar.", "将导出的纹理保存为 PNG 而非原始格式。PNG 很方便，但编码较慢。", "將匯出的紋理儲存為 PNG 而非原始格式。PNG 很方便，但編碼較慢。"},
    {"Enables Vita3K's experimental frame-rate hack. It can alter game speed or timing in unsupported titles.", "Active le hack expérimental de fréquence d'images de Vita3K. Il peut modifier la vitesse ou le timing des jeux non pris en charge.", "Aktiviert Vita3Ks experimentellen Bildraten-Hack. Bei nicht unterstützten Spielen kann er Geschwindigkeit oder Timing verändern.", "Activa el hack experimental de tasa de fotogramas de Vita3K. Puede alterar la velocidad o los tiempos de juegos no compatibles.", "Abilita l'hack sperimentale del frame rate di Vita3K. Può alterare velocità o temporizzazione nei giochi non supportati.", "Ativa o hack experimental de taxa de fotogramas do Vita3K. Pode alterar a velocidade ou temporização de jogos não suportados.", "启用 Vita3K 的实验性帧率 hack。它可能改变不受支持游戏的运行速度或时序。", "啟用 Vita3K 的實驗性幀率 hack。它可能改變不受支援遊戲的執行速度或時序。"},
    {"Shows the emulator performance overlay while a game is running.", "Affiche l'overlay de performances de l'émulateur pendant l'exécution d'un jeu.", "Zeigt während des Spiels das Leistungs-Overlay des Emulators.", "Muestra la superposición de rendimiento del emulador durante el juego.", "Mostra l'overlay delle prestazioni dell'emulatore durante il gioco.", "Mostra a sobreposição de desempenho do emulador durante o jogo.", "在游戏运行时显示模拟器性能覆盖层。", "在遊戲執行時顯示模擬器效能覆疊層。"},
    {"Controls how much timing and performance information the overlay displays.", "Règle la quantité d'informations de timing et de performances affichées par l'overlay.", "Steuert, wie viele Timing- und Leistungsdaten das Overlay anzeigt.", "Controla cuánta información de tiempos y rendimiento muestra la superposición.", "Controlla quante informazioni su tempi e prestazioni mostra l'overlay.", "Controla a quantidade de informação de temporização e desempenho mostrada pela sobreposição.", "控制覆盖层显示多少时序和性能信息。", "控制覆疊層顯示多少時序和效能資訊。"},
    {"Places the performance overlay in a screen corner or along the top or bottom edge.", "Place l'overlay de performances dans un coin de l'écran ou le long du bord supérieur ou inférieur.", "Platziert das Leistungs-Overlay in einer Bildschirmecke oder am oberen bzw. unteren Rand.", "Coloca la superposición de rendimiento en una esquina o en el borde superior o inferior.", "Posiziona l'overlay delle prestazioni in un angolo o lungo il bordo superiore o inferiore.", "Posiciona a sobreposição de desempenho num canto ou junto à margem superior ou inferior.", "将性能覆盖层放置在屏幕角落或顶部/底部边缘。", "將效能覆疊層放置在螢幕角落或頂部/底部邊緣。"},
    {"Sets Vita3K's output volume before it reaches the Switch system volume.", "Règle le volume de sortie de Vita3K avant le volume système de la Switch.", "Legt Vita3Ks Ausgangslautstärke vor der Switch-Systemlautstärke fest.", "Ajusta el volumen de salida de Vita3K antes del volumen del sistema Switch.", "Imposta il volume di uscita di Vita3K prima del volume di sistema di Switch.", "Define o volume de saída do Vita3K antes do volume do sistema da Switch.", "在到达 Switch 系统音量之前设置 Vita3K 的输出音量。", "在到達 Switch 系統音量之前設定 Vita3K 的輸出音量。"},
    {"Enables emulation of the Vita NGS audio engine used by many games.", "Active l'émulation du moteur audio NGS de la Vita utilisé par de nombreux jeux.", "Aktiviert die Emulation der von vielen Spielen genutzten Vita-NGS-Audioengine.", "Activa la emulación del motor de audio NGS de Vita usado por muchos juegos.", "Abilita l'emulazione del motore audio NGS di Vita usato da molti giochi.", "Ativa a emulação do motor de áudio NGS da Vita usado por muitos jogos.", "启用许多游戏使用的 Vita NGS 音频引擎模拟。", "啟用許多遊戲使用的 Vita NGS 音訊引擎模擬。"},
    {"Sets the language reported to Vita software. Games that support it may choose matching text and audio.", "Règle la langue indiquée aux logiciels Vita. Les jeux compatibles peuvent choisir les textes et l'audio correspondants.", "Legt die an Vita-Software gemeldete Sprache fest. Unterstützte Spiele können passende Texte und Audiospuren wählen.", "Ajusta el idioma indicado al software de Vita. Los juegos compatibles pueden usar texto y audio correspondientes.", "Imposta la lingua comunicata al software Vita. I giochi compatibili possono scegliere testo e audio corrispondenti.", "Define o idioma comunicado ao software Vita. Jogos compatíveis podem escolher texto e áudio correspondentes.", "设置报告给 Vita 软件的语言。支持的游戏可能会选择匹配的文本和音频。", "設定報告給 Vita 軟體的語言。支援的遊戲可能會選擇相符的文字和音訊。"},
    {"Chooses whether Cross or Circle is reported as the Vita system confirmation button.", "Choisit si Croix ou Cercle est indiqué comme bouton de confirmation du système Vita.", "Wählt, ob Kreuz oder Kreis als Bestätigungstaste des Vita-Systems gemeldet wird.", "Elige si Cruz o Círculo se usa como botón de confirmación del sistema Vita.", "Sceglie se Croce o Cerchio viene indicato come tasto di conferma del sistema Vita.", "Seleciona se Cruz ou Círculo é comunicado como botão de confirmação do sistema Vita.", "选择将叉号还是圆圈报告为 Vita 系统的确认按钮。", "選擇將叉號還是圓圈報告為 Vita 系統的確認按鈕。"},
    {"Sets the date format exposed through Vita system parameters.", "Règle le format de date exposé par les paramètres système Vita.", "Legt das über Vita-Systemparameter bereitgestellte Datumsformat fest.", "Ajusta el formato de fecha expuesto mediante los parámetros del sistema Vita.", "Imposta il formato data esposto tramite i parametri di sistema Vita.", "Define o formato de data exposto pelos parâmetros do sistema Vita.", "设置通过 Vita 系统参数暴露的日期格式。", "設定透過 Vita 系統參數暴露的日期格式。"},
    {"Sets the 12-hour or 24-hour clock format exposed to games.", "Règle le format d'horloge 12 ou 24 heures exposé aux jeux.", "Legt das für Spiele bereitgestellte 12- oder 24-Stunden-Zeitformat fest.", "Ajusta el formato de reloj de 12 o 24 horas expuesto a los juegos.", "Imposta il formato orario a 12 o 24 ore esposto ai giochi.", "Define o formato de relógio de 12 ou 24 horas exposto aos jogos.", "设置暴露给游戏的 12 小时或 24 小时制时钟格式。", "設定暴露給遊戲的 12 小時或 24 小時制時鐘格式。"},
    {"Reports a PlayStation TV environment to software. Some games change controls or block unsupported modes.", "Indique aux logiciels un environnement PlayStation TV. Certains jeux modifient leurs commandes ou bloquent les modes non pris en charge.", "Meldet der Software eine PlayStation-TV-Umgebung. Manche Spiele ändern die Steuerung oder sperren nicht unterstützte Modi.", "Informa al software de un entorno PlayStation TV. Algunos juegos cambian los controles o bloquean modos no compatibles.", "Comunica al software un ambiente PlayStation TV. Alcuni giochi cambiano i controlli o bloccano modalità non supportate.", "Comunica ao software um ambiente PlayStation TV. Alguns jogos alteram os controlos ou bloqueiam modos não suportados.", "向软件报告 PlayStation TV 环境。某些游戏会更改操控或阻止不受支持的模式。", "向軟體報告 PlayStation TV 環境。某些遊戲會變更操控或封鎖不受支援的模式。"},
    {"Allows Vita software to use Vita3K's HTTP networking implementation.", "Autorise les logiciels Vita à utiliser l'implémentation réseau HTTP de Vita3K.", "Erlaubt Vita-Software, Vita3Ks HTTP-Netzwerkimplementierung zu nutzen.", "Permite que el software de Vita use la implementación de red HTTP de Vita3K.", "Consente al software Vita di usare l'implementazione di rete HTTP di Vita3K.", "Permite ao software Vita usar a implementação de rede HTTP do Vita3K.", "允许 Vita 软件使用 Vita3K 的 HTTP 网络实现。", "允許 Vita 軟體使用 Vita3K 的 HTTP 網路實作。"},
    {"Sets how many polling attempts an HTTP operation receives before timing out.", "Règle le nombre de tentatives d'interrogation d'une opération HTTP avant expiration.", "Legt die Anzahl der HTTP-Abfrageversuche bis zum Timeout fest.", "Ajusta cuántos intentos de consulta tiene una operación HTTP antes de agotar el tiempo.", "Imposta il numero di tentativi di interrogazione HTTP prima del timeout.", "Define o número de tentativas de consulta de uma operação HTTP antes de expirar.", "设置 HTTP 操作在超时前获得多少次轮询尝试。", "設定 HTTP 操作在逾時前獲得多少次輪詢嘗試。"},
    {"Sets the delay between HTTP timeout polling attempts.", "Règle le délai entre les tentatives d'interrogation du délai HTTP.", "Legt die Pause zwischen HTTP-Timeout-Abfragen fest.", "Ajusta el intervalo entre intentos de consulta de tiempo de espera HTTP.", "Imposta l'intervallo tra i tentativi di timeout HTTP.", "Define o intervalo entre tentativas de timeout HTTP.", "设置 HTTP 超时轮询尝试之间的延迟。", "設定 HTTP 逾時輪詢嘗試之間的延遲。"},
    {"Sets how many times Vita3K checks for the end of an HTTP response.", "Règle le nombre de vérifications de fin de réponse HTTP effectuées par Vita3K.", "Legt fest, wie oft Vita3K das Ende einer HTTP-Antwort prüft.", "Ajusta cuántas veces Vita3K comprueba el final de una respuesta HTTP.", "Imposta quante volte Vita3K controlla la fine di una risposta HTTP.", "Define quantas vezes o Vita3K verifica o fim de uma resposta HTTP.", "设置 Vita3K 检查 HTTP 响应结束的次数。", "設定 Vita3K 檢查 HTTP 回應結束的次數。"},
    {"Sets the delay between HTTP response-end checks.", "Règle le délai entre les vérifications de fin de réponse HTTP.", "Legt die Pause zwischen Prüfungen auf das Ende einer HTTP-Antwort fest.", "Ajusta el intervalo entre comprobaciones del final de una respuesta HTTP.", "Imposta l'intervallo tra i controlli di fine risposta HTTP.", "Define o intervalo entre verificações do fim de uma resposta HTTP.", "设置 HTTP 响应结束检查之间的延迟。", "設定 HTTP 回應結束檢查之間的延遲。"},
    {"Reports a signed-in PSN state to games. It does not sign the console into PlayStation Network.", "Indique aux jeux une connexion PSN active. Cela ne connecte pas la console au PlayStation Network.", "Meldet Spielen einen angemeldeten PSN-Status. Die Konsole wird dadurch nicht beim PlayStation Network angemeldet.", "Informa a los juegos de una sesión PSN iniciada. No conecta la consola a PlayStation Network.", "Comunica ai giochi uno stato PSN connesso. Non accede al PlayStation Network dalla console.", "Comunica aos jogos um estado PSN com sessão iniciada. Não liga a consola à PlayStation Network.", "向游戏报告已登录 PSN 的状态。它不会将主机登录到 PlayStation Network。", "向遊戲報告已登入 PSN 的狀態。它不會將主機登入 PlayStation Network。"},
    {"Selects the local address index used by Vita ad-hoc networking.", "Sélectionne l'index d'adresse locale utilisé par le réseau ad hoc Vita.", "Wählt den lokalen Adressindex für Vita-Ad-hoc-Netzwerke.", "Selecciona el índice de dirección local usado por la red ad hoc de Vita.", "Seleziona l'indice dell'indirizzo locale usato dalla rete ad hoc Vita.", "Seleciona o índice do endereço local usado pela rede ad hoc Vita.", "选择 Vita 临时网络使用的本地地址索引。", "選擇 Vita 臨時網路使用的本機位址索引。"},
    {"Disables Vita motion-sensor input derived from the active Switch controller.", "Désactive les capteurs de mouvement Vita issus de la manette Switch active.", "Deaktiviert Vita-Bewegungssensordaten vom aktiven Switch-Controller.", "Desactiva la entrada de movimiento de Vita procedente del mando Switch activo.", "Disabilita l'input dei sensori di movimento Vita derivato dal controller Switch attivo.", "Desativa a entrada dos sensores de movimento Vita proveniente do comando Switch ativo.", "禁用来自当前 Switch 手柄的 Vita 运动传感器输入。", "停用來自目前 Switch 手把的 Vita 運動感應器輸入。"},
    {"Scales analog stick movement before it is sent to the emulated Vita.", "Met à l'échelle le mouvement du stick analogique avant son envoi à la Vita émulée.", "Skaliert Analogstick-Bewegungen, bevor sie an die emulierte Vita gesendet werden.", "Escala el movimiento del stick analógico antes de enviarlo a la Vita emulada.", "Scala il movimento dello stick analogico prima di inviarlo alla Vita emulata.", "Dimensiona o movimento do analógico antes de o enviar à Vita emulada.", "在将模拟摇杆移动发送到模拟的 Vita 之前对其进行缩放。", "在將類比搖桿移動傳送到模擬的 Vita 之前對其進行縮放。"},
    {"Ignores small stick movements to reduce drift. Too high a value reduces fine control.", "Ignore les petits mouvements du stick pour réduire le drift. Une valeur trop élevée réduit la précision.", "Ignoriert kleine Stickbewegungen gegen Drift. Ein zu hoher Wert verringert die Feinsteuerung.", "Ignora pequeños movimientos del stick para reducir el drift. Un valor alto reduce el control preciso.", "Ignora piccoli movimenti dello stick per ridurre il drift. Un valore troppo alto riduce la precisione.", "Ignora pequenos movimentos do analógico para reduzir drift. Um valor demasiado alto reduz o controlo preciso.", "忽略小幅摇杆移动以减少漂移。过高的值会降低精细操控。", "忽略小幅搖桿移動以減少漂移。過高的值會降低精細操控。"},
    {"Chooses the shoulder-button modifier used with the touchscreen to emulate the Vita rear touch panel.", "Choisit la gâchette utilisée avec l'écran tactile pour émuler le pavé tactile arrière de la Vita.", "Wählt die Schultertasten-Umschaltung, die zusammen mit dem Touchscreen das rückseitige Vita-Touchpad emuliert.", "Elige el modificador de hombro usado con la pantalla táctil para emular el panel táctil trasero de Vita.", "Sceglie il modificatore dorsale usato con il touchscreen per emulare il touch posteriore Vita.", "Seleciona o modificador de ombro usado com o ecrã tátil para emular o painel tátil traseiro da Vita.", "选择与触摸屏配合使用的肩键修饰键，以模拟 Vita 后触控板。", "選擇與觸控螢幕配合使用的肩鍵修飾鍵，以模擬 Vita 後觸控板。"},
    {"Chooses which Vita face button is produced by Nintendo Switch A.", "Choisit le bouton Vita produit par le bouton A de la Nintendo Switch.", "Wählt, welche Vita-Aktionstaste Nintendo Switch A erzeugt.", "Elige qué botón frontal de Vita produce Nintendo Switch A.", "Sceglie quale tasto frontale Vita viene prodotto da Nintendo Switch A.", "Seleciona qual botão frontal Vita é produzido pelo A da Nintendo Switch.", "选择 Nintendo Switch A 对应哪个 Vita 正面按钮。", "選擇 Nintendo Switch A 對應哪個 Vita 正面按鈕。"},
    {"Chooses which Vita face button is produced by Nintendo Switch B.", "Choisit le bouton Vita produit par le bouton B de la Nintendo Switch.", "Wählt, welche Vita-Aktionstaste Nintendo Switch B erzeugt.", "Elige qué botón frontal de Vita produce Nintendo Switch B.", "Sceglie quale tasto frontale Vita viene prodotto da Nintendo Switch B.", "Seleciona qual botão frontal Vita é produzido pelo B da Nintendo Switch.", "选择 Nintendo Switch B 对应哪个 Vita 正面按钮。", "選擇 Nintendo Switch B 對應哪個 Vita 正面按鈕。"},
    {"Chooses which Vita face button is produced by Nintendo Switch X.", "Choisit le bouton Vita produit par le bouton X de la Nintendo Switch.", "Wählt, welche Vita-Aktionstaste Nintendo Switch X erzeugt.", "Elige qué botón frontal de Vita produce Nintendo Switch X.", "Sceglie quale tasto frontale Vita viene prodotto da Nintendo Switch X.", "Seleciona qual botão frontal Vita é produzido pelo X da Nintendo Switch.", "选择 Nintendo Switch X 对应哪个 Vita 正面按钮。", "選擇 Nintendo Switch X 對應哪個 Vita 正面按鈕。"},
    {"Chooses which Vita face button is produced by Nintendo Switch Y.", "Choisit le bouton Vita produit par le bouton Y de la Nintendo Switch.", "Wählt, welche Vita-Aktionstaste Nintendo Switch Y erzeugt.", "Elige qué botón frontal de Vita produce Nintendo Switch Y.", "Sceglie quale tasto frontale Vita viene prodotto da Nintendo Switch Y.", "Seleciona qual botão frontal Vita é produzido pelo Y da Nintendo Switch.", "选择 Nintendo Switch Y 对应哪个 Vita 正面按钮。", "選擇 Nintendo Switch Y 對應哪個 Vita 正面按鈕。"},
    {"Selects the launcher background and color treatment. XMB, Bubbles, and Glow include optional animation.", "Sélectionne l'arrière-plan et les couleurs du lanceur. XMB, Bubbles et Glow proposent des animations facultatives.", "Wählt Hintergrund und Farbgestaltung des Launchers. XMB, Bubbles und Glow bieten optionale Animationen.", "Selecciona el fondo y los colores del lanzador. XMB, Bubbles y Glow incluyen animación opcional.", "Seleziona lo sfondo e i colori del launcher. XMB, Bubbles e Glow includono animazioni opzionali.", "Seleciona o fundo e as cores do launcher. XMB, Bubbles e Glow incluem animação opcional.", "选择启动器背景和配色方案。XMB、Bubbles 和 Glow 包含可选动画。", "選擇啟動器背景和配色方案。XMB、Bubbles 和 Glow 包含可選動畫。"},
    {"Rotates the complete launcher interface and touch coordinates in 90-degree steps.", "Fait pivoter toute l'interface du lanceur et les coordonnées tactiles par pas de 90 degrés.", "Dreht die gesamte Launcher-Oberfläche und Touch-Koordinaten in 90-Grad-Schritten.", "Gira toda la interfaz del lanzador y las coordenadas táctiles en pasos de 90 grados.", "Ruota l'intera interfaccia del launcher e le coordinate touch a passi di 90 gradi.", "Roda toda a interface do launcher e as coordenadas táteis em incrementos de 90 graus.", "以 90 度为步长旋转整个启动器界面和触摸坐标。", "以 90 度為步長旋轉整個啟動器介面和觸控座標。"},
    {"Sets the number of game covers shown across each library page.", "Règle le nombre de jaquettes affichées horizontalement sur chaque page de la bibliothèque.", "Legt die Anzahl der Spielcover pro Zeile jeder Bibliotheksseite fest.", "Ajusta el número de carátulas mostradas horizontalmente en cada página de la biblioteca.", "Imposta il numero di copertine mostrate orizzontalmente in ogni pagina della libreria.", "Define o número de capas mostradas horizontalmente em cada página da biblioteca.", "设置每个游戏库页面横向显示的游戏封面数量。", "設定每個遊戲庫頁面橫向顯示的遊戲封面數量。"},
    {"Sets the number of cover rows shown on each library page.", "Règle le nombre de lignes de jaquettes affichées sur chaque page de la bibliothèque.", "Legt die Anzahl der Cover-Zeilen pro Bibliotheksseite fest.", "Ajusta el número de filas de carátulas en cada página de la biblioteca.", "Imposta il numero di righe di copertine in ogni pagina della libreria.", "Define o número de linhas de capas em cada página da biblioteca.", "设置每个游戏库页面显示的封面行数。", "設定每個遊戲庫頁面顯示的封面行數。"},
    {"Shows or hides game names below cover artwork.", "Affiche ou masque le nom des jeux sous leur jaquette.", "Zeigt oder verbirgt Spielnamen unter dem Cover.", "Muestra u oculta los nombres de los juegos bajo las carátulas.", "Mostra o nasconde i nomi dei giochi sotto le copertine.", "Mostra ou oculta os nomes dos jogos sob as capas.", "显示或隐藏封面下方的游戏名称。", "顯示或隱藏封面下方的遊戲名稱。"},
    {"Shows or hides the region flag in the top-left corner of each game cover.", "Affiche ou masque le drapeau de région dans le coin supérieur gauche de chaque jaquette.", "Blendet die Regionsflagge oben links auf jedem Spielcover ein oder aus.", "Muestra u oculta la bandera de región en la esquina superior izquierda de cada carátula.", "Mostra o nasconde la bandiera regionale nell'angolo superiore sinistro di ogni copertina.", "Mostra ou oculta a bandeira de região no canto superior esquerdo de cada capa.", "显示或隐藏每个游戏封面左上角的区域旗帜。", "顯示或隱藏每個遊戲封面左上角的區域旗幟。"},
    {"Shows or hides the square badge on games that have per-game settings. The settings themselves are not changed.", "Affiche ou masque l'indicateur carré sur les jeux ayant des paramètres par jeu. Les paramètres eux-mêmes ne sont pas modifiés.", "Blendet die quadratische Markierung bei Spielen mit spielspezifischen Einstellungen ein oder aus. Die Einstellungen selbst werden nicht geändert.", "Muestra u oculta el indicador cuadrado en los juegos con ajustes por juego. Los ajustes no se modifican.", "Mostra o nasconde l'indicatore quadrato sui giochi con impostazioni specifiche. Le impostazioni non vengono modificate.", "Mostra ou oculta o indicador quadrado nos jogos com definições por jogo. As definições não são alteradas.", "在具有游戏专属设置的游戏上显示或隐藏方形徽标。设置本身不会更改。", "在具有遊戲專屬設定的遊戲上顯示或隱藏方形徽章。設定本身不會變更。"},
    {"Enables animated backgrounds, fades, highlight easing, and cover transitions.", "Active les arrière-plans animés, fondus, déplacements fluides de sélection et transitions de jaquettes.", "Aktiviert animierte Hintergründe, Überblendungen, weiche Auswahlbewegung und Cover-Übergänge.", "Activa fondos animados, fundidos, movimiento suave del resaltado y transiciones de carátulas.", "Abilita sfondi animati, dissolvenze, movimento fluido della selezione e transizioni delle copertine.", "Ativa fundos animados, desvanecimentos, movimento suave da seleção e transições das capas.", "启用动画背景、淡入淡出、高亮缓动和封面过渡效果。", "啟用動畫背景、淡入淡出、高亮緩動和封面過渡效果。"},
    {"Enables navigation, confirmation, and back sound effects in the launcher.", "Active les sons de navigation, de confirmation et de retour du lanceur.", "Aktiviert Navigations-, Bestätigungs- und Zurück-Sounds im Launcher.", "Activa los sonidos de navegación, confirmación y retroceso del lanzador.", "Abilita i suoni di navigazione, conferma e ritorno nel launcher.", "Ativa os sons de navegação, confirmação e retrocesso no launcher.", "启用启动器中的导航、确认和返回音效。", "啟用啟動器中的導覽、確認和返回音效。"},
    {"Checks the official Vita3K-nx GitHub releases in the background when the library opens. Direct HOME shortcut launches skip the check.", "Vérifie en arrière-plan les versions officielles de Vita3K-nx sur GitHub à l'ouverture de la bibliothèque. Les raccourcis HOME directs ignorent cette vérification.", "Prüft beim Öffnen der Bibliothek im Hintergrund die offiziellen Vita3K-nx-GitHub-Versionen. Direkte HOME-Verknüpfungen überspringen die Prüfung.", "Busca en segundo plano versiones oficiales de Vita3K-nx en GitHub al abrir la biblioteca. Los accesos directos HOME omiten la comprobación.", "Controlla in background le versioni ufficiali Vita3K-nx su GitHub all'apertura della libreria. I collegamenti HOME diretti saltano il controllo.", "Verifica em segundo plano as versões oficiais do Vita3K-nx no GitHub ao abrir a biblioteca. Atalhos HOME diretos ignoram a verificação.", "在游戏库打开时于后台检查官方 Vita3K-nx GitHub 版本。直接通过 HOME 快捷方式启动会跳过检查。", "在遊戲庫開啟時於背景檢查官方 Vita3K-nx GitHub 版本。直接透過 HOME 捷徑啟動會跳過檢查。"},
    {"Changes only the SDL launcher's language. Vita system language is configured separately under System.", "Modifie uniquement la langue du lanceur SDL. La langue du système Vita se règle séparément dans Système.", "Ändert nur die Sprache des SDL-Launchers. Die Vita-Systemsprache wird separat unter System eingestellt.", "Cambia solo el idioma del lanzador SDL. El idioma del sistema Vita se configura por separado en Sistema.", "Cambia solo la lingua del launcher SDL. La lingua di sistema Vita si configura separatamente in Sistema.", "Altera apenas o idioma do launcher SDL. O idioma do sistema Vita é configurado separadamente em Sistema.", "仅更改 SDL 启动器的语言。Vita 系统语言在「系统」下单独配置。", "僅變更 SDL 啟動器的語言。Vita 系統語言在「系統」下單獨設定。"},
    {"Sets the SteamGridDB API key used for cover and HOME shortcut artwork searches. Leave it empty to remove the key.", "Règle la clé API SteamGridDB utilisée pour rechercher les jaquettes et icônes des raccourcis HOME. Laissez vide pour supprimer la clé.", "Legt den SteamGridDB-API-Schlüssel für Cover- und HOME-Verknüpfungsgrafiken fest. Leer lassen, um den Schlüssel zu entfernen.", "Ajusta la clave API de SteamGridDB usada para buscar carátulas e imágenes de accesos HOME. Déjala vacía para borrar la clave.", "Imposta la chiave API SteamGridDB usata per cercare copertine e immagini dei collegamenti HOME. Lascia vuoto per rimuoverla.", "Define a chave API SteamGridDB usada para procurar capas e imagens de atalhos HOME. Deixe vazio para remover a chave.", "设置用于封面和 HOME 快捷方式图片搜索的 SteamGridDB API 密钥。留空可删除该密钥。", "設定用於封面和 HOME 捷徑圖片搜尋的 SteamGridDB API 金鑰。留空可刪除該金鑰。"},
    {"System data / font", "Données système / police", "Systemdaten / Schriftart", "Datos del sistema / fuente", "Dati di sistema / carattere", "Dados do sistema / tipo de letra", "系统数据 / 字体", "系統資料 / 字型"},
    {"LSFG 2x (Vulkan only)", "LSFG 2x (Vulkan uniquement)", "LSFG 2x (nur Vulkan)", "LSFG 2x (solo Vulkan)", "LSFG 2x (solo Vulkan)", "LSFG 2x (apenas Vulkan)", "LSFG 2x（仅 Vulkan）", "LSFG 2x（僅 Vulkan）"},
    {"Flow resolution", "Résolution du flux", "Flussauflösung", "Resolución del flujo", "Risoluzione del flusso", "Resolução do fluxo", "光流分辨率", "光流解析度"},
    {"Performance mode", "Mode performances", "Leistungsmodus", "Modo rendimiento", "Modalità prestazioni", "Modo de desempenho", "性能模式", "效能模式"},
    {"Lossless.dll", "Lossless.dll", "Lossless.dll", "Lossless.dll", "Lossless.dll", "Lossless.dll", "Lossless.dll", "Lossless.dll"},
    {"Modules mode", "Mode des modules", "Modulmodus", "Modo de módulos", "Modalità moduli", "Modo de módulos", "模块模式", "模組模式"},
    {"CPU optimisations", "Optimisations CPU", "CPU-Optimierungen", "Optimizaciones de CPU", "Ottimizzazioni CPU", "Otimizações de CPU", "CPU 优化", "CPU 最佳化"},
    {"Renderer", "Moteur de rendu", "Renderer", "Renderizador", "Renderer", "Renderizador", "渲染器", "渲染器"},
    {"Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)", "Vulkan (NVK)"},
    {"OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)", "OpenGL (NVC0)"},
    {"Zink (OpenGL on NVK)", "Zink (OpenGL sur NVK)", "Zink (OpenGL über NVK)", "Zink (OpenGL sobre NVK)", "Zink (OpenGL su NVK)", "Zink (OpenGL sobre NVK)", "Zink（NVK 上的 OpenGL）", "Zink（NVK 上的 OpenGL）"},
    {"Vulkan only", "Vulkan uniquement", "Nur Vulkan", "Solo Vulkan", "Solo Vulkan", "Apenas Vulkan", "仅 Vulkan", "僅 Vulkan"},
    {"Resolution scale", "Échelle de résolution", "Auflösungsskalierung", "Escala de resolución", "Scala risoluzione", "Escala de resolução", "分辨率缩放", "解析度縮放"},
    {"Memory mapping", "Mappage mémoire", "Speicherabbildung", "Mapeo de memoria", "Mappatura memoria", "Mapeamento de memória", "内存映射", "記憶體映射"},
    {"High accuracy", "Haute précision", "Hohe Genauigkeit", "Alta precisión", "Alta precisione", "Alta precisão", "高精度", "高精度"},
    {"Screen filter", "Filtre d'écran", "Bildschirmfilter", "Filtro de pantalla", "Filtro schermo", "Filtro de ecrã", "屏幕滤镜", "螢幕濾鏡"},
    {"VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync", "VSync"},
    {"Anisotropic filtering", "Filtrage anisotrope", "Anisotrope Filterung", "Filtrado anisotrópico", "Filtro anisotropico", "Filtragem anisotrópica", "各向异性过滤", "各向異性過濾"},
    {"Disable surface sync", "Désactiver la synchronisation des surfaces", "Oberflächensynchronisierung deaktivieren", "Desactivar sincronización de superficies", "Disabilita sincronizzazione superfici", "Desativar sincronização de superfícies", "禁用表面同步", "停用表面同步"},
    {"Texture cache", "Cache des textures", "Textur-Cache", "Caché de texturas", "Cache texture", "Cache de texturas", "纹理缓存", "紋理快取"},
    {"Async pipeline compile", "Compilation asynchrone des pipelines", "Asynchrone Pipeline-Kompilierung", "Compilación asíncrona de canalizaciones", "Compilazione asincrona pipeline", "Compilação assíncrona de pipelines", "异步管线编译", "非同步管線編譯"},
    {"Show compiling shaders", "Afficher la compilation des shaders", "Shader-Kompilierung anzeigen", "Mostrar compilación de shaders", "Mostra compilazione shader", "Mostrar compilação de shaders", "显示着色器编译", "顯示著色器編譯"},
    {"Shader cache", "Cache des shaders", "Shader-Cache", "Caché de shaders", "Cache shader", "Cache de shaders", "着色器缓存", "著色器快取"},
    {"Import textures", "Importer les textures", "Texturen importieren", "Importar texturas", "Importa texture", "Importar texturas", "导入纹理", "匯入紋理"},
    {"Export textures", "Exporter les textures", "Texturen exportieren", "Exportar texturas", "Esporta texture", "Exportar texturas", "导出纹理", "匯出紋理"},
    {"Export as PNG", "Exporter en PNG", "Als PNG exportieren", "Exportar como PNG", "Esporta come PNG", "Exportar como PNG", "导出为 PNG", "匯出為 PNG"},
    {"FPS hack", "Hack FPS", "FPS-Hack", "Hack de FPS", "Hack FPS", "Hack de FPS", "FPS hack", "FPS hack"},
    {"Performance overlay", "Overlay de performances", "Leistungs-Overlay", "Superposición de rendimiento", "Overlay prestazioni", "Sobreposição de desempenho", "性能覆盖层", "效能覆疊層"},
    {"Overlay detail", "Détails de l'overlay", "Overlay-Details", "Detalle de la superposición", "Dettaglio overlay", "Detalhe da sobreposição", "覆盖层详细程度", "覆疊層詳細程度"},
    {"Overlay position", "Position de l'overlay", "Overlay-Position", "Posición de la superposición", "Posizione overlay", "Posição da sobreposição", "覆盖层位置", "覆疊層位置"},
    {"Audio volume", "Volume audio", "Audiolautstärke", "Volumen de audio", "Volume audio", "Volume do áudio", "音频音量", "音訊音量"},
    {"NGS audio support", "Prise en charge audio NGS", "NGS-Audiounterstützung", "Compatibilidad de audio NGS", "Supporto audio NGS", "Suporte de áudio NGS", "NGS 音频支持", "NGS 音訊支援"},
    {"System language", "Langue du système", "Systemsprache", "Idioma del sistema", "Lingua di sistema", "Idioma do sistema", "系统语言", "系統語言"},
    {"Enter button", "Bouton de validation", "Bestätigungstaste", "Botón de confirmación", "Tasto di conferma", "Botão de confirmação", "确认按钮", "確認按鈕"},
    {"Date format", "Format de date", "Datumsformat", "Formato de fecha", "Formato data", "Formato de data", "日期格式", "日期格式"},
    {"Time format", "Format de l'heure", "Zeitformat", "Formato de hora", "Formato ora", "Formato da hora", "时间格式", "時間格式"},
    {"PS TV mode", "Mode PS TV", "PS-TV-Modus", "Modo PS TV", "Modalità PS TV", "Modo PS TV", "PS TV 模式", "PS TV 模式"},
    {"HTTP enable", "Activer HTTP", "HTTP aktivieren", "Activar HTTP", "Abilita HTTP", "Ativar HTTP", "启用 HTTP", "啟用 HTTP"},
    {"HTTP timeout attempts", "Tentatives avant expiration HTTP", "HTTP-Timeout-Versuche", "Intentos de espera HTTP", "Tentativi timeout HTTP", "Tentativas de timeout HTTP", "HTTP 超时尝试次数", "HTTP 逾時嘗試次數"},
    {"HTTP timeout sleep (ms)", "Attente du délai HTTP (ms)", "HTTP-Timeout-Pause (ms)", "Espera de tiempo HTTP (ms)", "Pausa timeout HTTP (ms)", "Pausa de timeout HTTP (ms)", "HTTP 超时等待（毫秒）", "HTTP 逾時等待（毫秒）"},
    {"HTTP read-end attempts", "Tentatives de fin de lecture HTTP", "HTTP-Leseende-Versuche", "Intentos de fin de lectura HTTP", "Tentativi fine lettura HTTP", "Tentativas de fim de leitura HTTP", "HTTP 读取结束尝试次数", "HTTP 讀取結束嘗試次數"},
    {"HTTP read-end sleep (ms)", "Attente de fin de lecture HTTP (ms)", "HTTP-Leseende-Pause (ms)", "Espera de fin de lectura HTTP (ms)", "Pausa fine lettura HTTP (ms)", "Pausa de fim de leitura HTTP (ms)", "HTTP 读取结束等待（毫秒）", "HTTP 讀取結束等待（毫秒）"},
    {"PSN signed in", "Connexion PSN active", "Bei PSN angemeldet", "Sesión PSN iniciada", "Accesso PSN effettuato", "Sessão PSN iniciada", "PSN 已登录", "PSN 已登入"},
    {"Ad-hoc address index", "Index d'adresse ad hoc", "Ad-hoc-Adressindex", "Índice de dirección ad hoc", "Indice indirizzo ad hoc", "Índice de endereço ad hoc", "临时网络地址索引", "臨時網路位址索引"},
    {"Disable motion", "Désactiver les mouvements", "Bewegung deaktivieren", "Desactivar movimiento", "Disabilita movimento", "Desativar movimento", "禁用运动", "停用運動"},
    {"Stick sensitivity", "Sensibilité du stick", "Stick-Empfindlichkeit", "Sensibilidad del stick", "Sensibilità stick", "Sensibilidade do analógico", "摇杆灵敏度", "搖桿靈敏度"},
    {"Stick deadzone (%)", "Zone morte du stick (%)", "Stick-Totzone (%)", "Zona muerta del stick (%)", "Zona morta stick (%)", "Zona morta do analógico (%)", "摇杆死区（%）", "搖桿死區（%）"},
    {"Rear touch modifier", "Modificateur tactile arrière", "Rückseiten-Touch-Modifikator", "Modificador táctil trasero", "Modificatore touch posteriore", "Modificador tátil traseiro", "后触控修饰键", "後觸控修飾鍵"},
    {"Switch A maps to", "Bouton Switch A associé à", "Switch A entspricht", "Switch A se asigna a", "Switch A corrisponde a", "Switch A corresponde a", "Switch A 映射到", "Switch A 對應到"},
    {"Switch B maps to", "Bouton Switch B associé à", "Switch B entspricht", "Switch B se asigna a", "Switch B corrisponde a", "Switch B corresponde a", "Switch B 映射到", "Switch B 對應到"},
    {"Switch X maps to", "Bouton Switch X associé à", "Switch X entspricht", "Switch X se asigna a", "Switch X corrisponde a", "Switch X corresponde a", "Switch X 映射到", "Switch X 對應到"},
    {"Switch Y maps to", "Bouton Switch Y associé à", "Switch Y entspricht", "Switch Y se asigna a", "Switch Y corrisponde a", "Switch Y corresponde a", "Switch Y 映射到", "Switch Y 對應到"},
    {"Launcher rotation", "Rotation du lanceur", "Launcher-Drehung", "Rotación del lanzador", "Rotazione launcher", "Rotação do launcher", "启动器旋转", "啟動器旋轉"},
    {"SteamGridDB API key", "Clé API SteamGridDB", "SteamGridDB-API-Schlüssel", "Clave API de SteamGridDB", "Chiave API SteamGridDB", "Chave API SteamGridDB", "SteamGridDB API 密钥", "SteamGridDB API 金鑰"},
    {"0.5x", "0.5x", "0.5x", "0.5x", "0.5x", "0.5x", "0.5x", "0.5x"},
    {"1.0x", "1.0x", "1.0x", "1.0x", "1.0x", "1.0x", "1.0x", "1.0x"},
    {"1.5x", "1.5x", "1.5x", "1.5x", "1.5x", "1.5x", "1.5x", "1.5x"},
    {"2.0x", "2.0x", "2.0x", "2.0x", "2.0x", "2.0x", "2.0x", "2.0x"},
    {"2x", "2x", "2x", "2x", "2x", "2x", "2x", "2x"},
    {"3.0x", "3.0x", "3.0x", "3.0x", "3.0x", "3.0x", "3.0x", "3.0x"},
    {"4x", "4x", "4x", "4x", "4x", "4x", "4x", "4x"},
    {"8x", "8x", "8x", "8x", "8x", "8x", "8x", "8x"},
    {"16x", "16x", "16x", "16x", "16x", "16x", "16x", "16x"},
    {"1", "1", "1", "1", "1", "1", "1", "1"},
    {"2", "2", "2", "2", "2", "2", "2", "2"},
    {"3", "3", "3", "3", "3", "3", "3", "3"},
    {"4", "4", "4", "4", "4", "4", "4", "4"},
    {"5", "5", "5", "5", "5", "5", "5", "5"},
    {"6", "6", "6", "6", "6", "6", "6", "6"},
    {"7", "7", "7", "7", "7", "7", "7", "7"},
    {"8", "8", "8", "8", "8", "8", "8", "8"},
    {"Automatic", "Automatique", "Automatisch", "Automático", "Automatico", "Automático", "自动", "自動"},
    {"Auto + manual", "Auto + manuel", "Auto + manuell", "Auto + manual", "Auto + manuale", "Auto + manual", "自动 + 手动", "自動 + 手動"},
    {"Manual", "Manuel", "Manuell", "Manual", "Manuale", "Manual", "手动", "手動"},
    {"Nearest", "Nearest", "Nearest", "Nearest", "Nearest", "Nearest", "最近邻", "最近鄰"},
    {"Bilinear", "Bilinear", "Bilinear", "Bilinear", "Bilineare", "Bilinear", "双线性", "雙線性"},
    {"Bicubic", "Bicubique", "Bikubisch", "Bicúbico", "Bicubico", "Bicúbico", "双三次", "雙三次"},
    {"FXAA", "FXAA", "FXAA", "FXAA", "FXAA", "FXAA", "FXAA", "FXAA"},
    {"FSR", "FSR", "FSR", "FSR", "FSR", "FSR", "FSR", "FSR"},
    {"Double buffer", "Double buffer", "Doppelpuffer", "Búfer doble", "Doppio buffer", "Buffer duplo", "双缓冲", "雙緩衝"},
    {"0 degrees", "0 degré", "0 Grad", "0 grados", "0 gradi", "0 graus", "0 度", "0 度"},
    {"90 degrees", "90 degrés", "90 Grad", "90 grados", "90 gradi", "90 graus", "90 度", "90 度"},
    {"180 degrees", "180 degrés", "180 Grad", "180 grados", "180 gradi", "180 graus", "180 度", "180 度"},
    {"270 degrees", "270 degrés", "270 Grad", "270 grados", "270 gradi", "270 graus", "270 度", "270 度"},
    {"XMB (PS3)", "XMB (PS3)", "XMB (PS3)", "XMB (PS3)", "XMB (PS3)", "XMB (PS3)", "XMB (PS3)", "XMB (PS3)"},
    {"Glow", "Lueur", "Leuchten", "Brillo", "Bagliore", "Brilho", "辉光", "輝光"},
    {"Bubbles", "Bulles", "Blasen", "Burbujas", "Bolle", "Bolhas", "气泡", "氣泡"},
    {"Classic", "Classique", "Klassisch", "Clásico", "Classico", "Clássico", "经典", "經典"},
    {"OLED black", "Noir OLED", "OLED-Schwarz", "Negro OLED", "Nero OLED", "Preto OLED", "OLED 黑", "OLED 黑"},
    {"Quarter", "Quart", "Viertel", "Cuarto", "Quarto", "Quarto", "四分之一", "四分之一"},
    {"Half", "Demi", "Halb", "Mitad", "Metà", "Metade", "二分之一", "二分之一"},
    {"Minimum", "Minimum", "Minimum", "Mínimo", "Minimo", "Mínimo", "最低", "最低"},
    {"Low", "Faible", "Niedrig", "Bajo", "Basso", "Baixo", "低", "低"},
    {"Medium", "Moyen", "Mittel", "Medio", "Medio", "Médio", "中", "中"},
    {"Maximum", "Maximum", "Maximum", "Máximo", "Massimo", "Máximo", "最高", "最高"},
    {"Top left", "En haut à gauche", "Oben links", "Arriba a la izquierda", "In alto a sinistra", "Superior esquerdo", "左上", "左上"},
    {"Top center", "En haut au centre", "Oben mittig", "Arriba en el centro", "In alto al centro", "Superior central", "上中", "上方中央"},
    {"Top right", "En haut à droite", "Oben rechts", "Arriba a la derecha", "In alto a destra", "Superior direito", "右上", "右上"},
    {"Bottom left", "En bas à gauche", "Unten links", "Abajo a la izquierda", "In basso a sinistra", "Inferior esquerdo", "左下", "左下"},
    {"Bottom center", "En bas au centre", "Unten mittig", "Abajo en el centro", "In basso al centro", "Inferior central", "下中", "下方中央"},
    {"Bottom right", "En bas à droite", "Unten rechts", "Abajo a la derecha", "In basso a destra", "Inferior direito", "右下", "右下"},
    {"Cross", "Croix", "Kreuz", "Cruz", "Croce", "Cruz", "叉号", "叉號"},
    {"Circle", "Cercle", "Kreis", "Círculo", "Cerchio", "Círculo", "圆圈", "圓圈"},
    {"Triangle", "Triangle", "Dreieck", "Triángulo", "Triangolo", "Triângulo", "三角形", "三角形"},
    {"Square", "Carré", "Quadrat", "Cuadrado", "Quadrato", "Quadrado", "方块", "方塊"},
    {"ZL + touchscreen", "ZL + écran tactile", "ZL + Touchscreen", "ZL + pantalla táctil", "ZL + touchscreen", "ZL + ecrã tátil", "ZL + 触摸屏", "ZL + 觸控螢幕"},
    {"ZR + touchscreen", "ZR + écran tactile", "ZR + Touchscreen", "ZR + pantalla táctil", "ZR + touchscreen", "ZR + ecrã tátil", "ZR + 触摸屏", "ZR + 觸控螢幕"},
    {"12-hour", "12 heures", "12 Stunden", "12 horas", "12 ore", "12 horas", "12 小时制", "12 小時制"},
    {"24-hour", "24 heures", "24 Stunden", "24 horas", "24 ore", "24 horas", "24 小时制", "24 小時制"},
    {"YYYY/MM/DD", "AAAA/MM/JJ", "JJJJ/MM/TT", "AAAA/MM/DD", "AAAA/MM/GG", "AAAA/MM/DD", "YYYY/MM/DD", "YYYY/MM/DD"},
    {"DD/MM/YYYY", "JJ/MM/AAAA", "TT/MM/JJJJ", "DD/MM/AAAA", "GG/MM/AAAA", "DD/MM/AAAA", "DD/MM/YYYY", "DD/MM/YYYY"},
    {"MM/DD/YYYY", "MM/JJ/AAAA", "MM/TT/JJJJ", "MM/DD/AAAA", "MM/GG/AAAA", "MM/DD/AAAA", "MM/DD/YYYY", "MM/DD/YYYY"},
    {"Japanese", "Japonais", "Japanisch", "Japonés", "Giapponese", "Japonês", "日语", "日語"},
    {"English", "Anglais", "Englisch", "Inglés", "Inglese", "Inglês", "英语", "英語"},
    {"English (US)", "Anglais (États-Unis)", "Englisch (USA)", "Inglés (EE. UU.)", "Inglese (USA)", "Inglês (EUA)", "英语（美国）", "英語（美國）"},
    {"English (UK)", "Anglais (Royaume-Uni)", "Englisch (GB)", "Inglés (Reino Unido)", "Inglese (Regno Unito)", "Inglês (Reino Unido)", "英语（英国）", "英語（英國）"},
    {"French", "Français", "Französisch", "Francés", "Francese", "Francês", "法语", "法語"},
    {"Spanish", "Espagnol", "Spanisch", "Español", "Spagnolo", "Espanhol", "西班牙语", "西班牙語"},
    {"German", "Allemand", "Deutsch", "Alemán", "Tedesco", "Alemão", "德语", "德語"},
    {"Italian", "Italien", "Italienisch", "Italiano", "Italiano", "Italiano", "意大利语", "義大利語"},
    {"Dutch", "Néerlandais", "Niederländisch", "Neerlandés", "Olandese", "Neerlandês", "荷兰语", "荷蘭語"},
    {"Portuguese", "Portugais", "Portugiesisch", "Portugués", "Portoghese", "Português", "葡萄牙语", "葡萄牙語"},
    {"Portuguese (BR)", "Portugais (Brésil)", "Portugiesisch (Brasilien)", "Portugués (Brasil)", "Portoghese (Brasile)", "Português (Brasil)", "葡萄牙语（巴西）", "葡萄牙語（巴西）"},
    {"Russian", "Russe", "Russisch", "Ruso", "Russo", "Russo", "俄语", "俄語"},
    {"Korean", "Coréen", "Koreanisch", "Coreano", "Coreano", "Coreano", "韩语", "韓語"},
    {"Chinese (Trad.)", "Chinois (traditionnel)", "Chinesisch (trad.)", "Chino (trad.)", "Cinese (trad.)", "Chinês (trad.)", "中文（繁体）", "中文（繁體）"},
    {"Chinese (Simp.)", "Chinois (simplifié)", "Chinesisch (vereinf.)", "Chino (simpl.)", "Cinese (sempl.)", "Chinês (simpl.)", "中文（简体）", "中文（簡體）"},
    {"Finnish", "Finnois", "Finnisch", "Finés", "Finlandese", "Finlandês", "芬兰语", "芬蘭語"},
    {"Swedish", "Suédois", "Schwedisch", "Sueco", "Svedese", "Sueco", "瑞典语", "瑞典語"},
    {"Danish", "Danois", "Dänisch", "Danés", "Danese", "Dinamarquês", "丹麦语", "丹麥語"},
    {"Norwegian", "Norvégien", "Norwegisch", "Noruego", "Norvegese", "Norueguês", "挪威语", "挪威語"},
    {"Polish", "Polonais", "Polnisch", "Polaco", "Polacco", "Polaco", "波兰语", "波蘭語"},
    {"Turkish", "Turc", "Türkisch", "Turco", "Turco", "Turco", "土耳其语", "土耳其語"},
    {"Français", "Français", "Französisch", "Francés", "Francese", "Francês", "法语", "法語"},
    {"Deutsch", "Allemand", "Deutsch", "Alemán", "Tedesco", "Alemão", "德语", "德語"},
    {"Español", "Espagnol", "Spanisch", "Español", "Spagnolo", "Espanhol", "西班牙语", "西班牙語"},
    {"Italiano", "Italien", "Italienisch", "Italiano", "Italiano", "Italiano", "意大利语", "義大利語"},
    {"Português", "Portugais", "Portugiesisch", "Portugués", "Portoghese", "Português", "葡萄牙语", "葡萄牙語"},
    {"Global Vita3K setting", "Réglage global Vita3K", "Globale Vita3K-Einstellung", "Ajuste global de Vita3K", "Impostazione globale Vita3K", "Definição global do Vita3K", "全局 Vita3K 设置", "全域 Vita3K 設定"},
    {"Per-game override", "Réglage propre au jeu", "Spielspezifische Überschreibung", "Ajuste específico del juego", "Override specifico del gioco", "Substituição específica do jogo", "游戏专属覆盖", "遊戲專屬覆寫"},
    {"Launcher setting", "Réglage du lanceur", "Launcher-Einstellung", "Ajuste del lanzador", "Impostazione launcher", "Definição do launcher", "启动器设置", "啟動器設定"},
    {"Launcher action", "Action du lanceur", "Launcher-Aktion", "Acción del lanzador", "Azione launcher", "Ação do launcher", "启动器操作", "啟動器動作"},
    {"Launcher settings", "Réglages du lanceur", "Launcher-Einstellungen", "Ajustes del lanzador", "Impostazioni launcher", "Definições do launcher", "启动器设置", "啟動器設定"},
    {"Launcher section", "Section du lanceur", "Launcher-Bereich", "Sección del lanzador", "Sezione launcher", "Secção do launcher", "启动器分区", "啟動器區段"},
    {"Launcher tools", "Outils du lanceur", "Launcher-Werkzeuge", "Herramientas del lanzador", "Strumenti launcher", "Ferramentas do launcher", "启动器工具", "啟動器工具"},
    {"Storage category", "Catégorie de stockage", "Speicherkategorie", "Categoría de almacenamiento", "Categoria archiviazione", "Categoria de armazenamento", "存储类别", "儲存類別"},
    {"Storage tools", "Outils de stockage", "Speicherwerkzeuge", "Herramientas de almacenamiento", "Strumenti di archiviazione", "Ferramentas de armazenamento", "存储工具", "儲存工具"},
    {"SMB configuration", "Configuration SMB", "SMB-Konfiguration", "Configuración SMB", "Configurazione SMB", "Configuração SMB", "SMB 配置", "SMB 設定"},
    {"Vita3K setting category", "Catégorie de réglages Vita3K", "Vita3K-Einstellungskategorie", "Categoría de ajustes de Vita3K", "Categoria impostazioni Vita3K", "Categoria de definições do Vita3K", "Vita3K 设置类别", "Vita3K 設定類別"},
    {"Global emulator settings", "Réglages globaux de l'émulateur", "Globale Emulator-Einstellungen", "Ajustes globales del emulador", "Impostazioni globali emulatore", "Definições globais do emulador", "全局模拟器设置", "全域模擬器設定"},
    {"Game override", "Réglage du jeu", "Spielüberschreibung", "Ajuste del juego", "Override del gioco", "Substituição do jogo", "游戏覆盖", "遊戲覆寫"},
    {"Safely eject", "Éjecter en toute sécurité", "Sicher auswerfen", "Expulsar de forma segura", "Espelli in sicurezza", "Ejetar com segurança", "安全弹出", "安全退出"},
    {"USB drive ejected safely", "Périphérique USB éjecté en toute sécurité", "USB-Laufwerk sicher ausgeworfen", "Unidad USB expulsada de forma segura", "Unità USB espulsa in sicurezza", "Unidade USB ejetada em segurança", "USB 设备已安全弹出", "USB 裝置已安全退出"},
    {"Cancel", "Annuler", "Abbrechen", "Cancelar", "Annulla", "Cancelar", "取消", "取消"},
    {"Cancelling...", "Annulation...", "Wird abgebrochen...", "Cancelando...", "Annullamento...", "A cancelar...", "正在取消...", "正在取消..."},
    {"Page", "Page", "Seite", "Página", "Pagina", "Página", "页", "頁"},
    {"Sort:", "Tri :", "Sortierung:", "Orden:", "Ordine:", "Ordenação:", "排序：", "排序："},
    {"Recently played", "Joués récemment", "Kürzlich gespielt", "Jugados recientemente", "Giocati di recente", "Jogados recentemente", "最近游玩", "最近遊玩"},
    {"Recently added", "Ajoutés récemment", "Kürzlich hinzugefügt", "Añadidos recientemente", "Aggiunti di recente", "Adicionados recentemente", "最近添加", "最近新增"},
    {"Launch", "Lancer", "Starten", "Iniciar", "Avvia", "Iniciar", "启动", "啟動"},
    {"Rename game", "Renommer le jeu", "Spiel umbenennen", "Renombrar juego", "Rinomina gioco", "Renomear jogo", "重命名游戏", "重新命名遊戲"},
    {"Create HOME shortcut", "Créer un raccourci HOME", "HOME-Verknüpfung erstellen", "Crear acceso directo HOME", "Crea collegamento HOME", "Criar atalho HOME", "创建 HOME 快捷方式", "建立 HOME 捷徑"},
    {"Favorites & collections", "Favoris et collections", "Favoriten & Sammlungen", "Favoritos y colecciones", "Preferiti e raccolte", "Favoritos e coleções", "收藏与合集", "收藏與合集"},
    {"Clear cache", "Vider le cache", "Cache leeren", "Borrar caché", "Svuota cache", "Limpar cache", "清除缓存", "清除快取"},
    {"Clear per-game settings", "Effacer les paramètres par jeu", "Spielspezifische Einstellungen löschen", "Borrar ajustes por juego", "Cancella impostazioni per gioco", "Limpar definições por jogo", "清除游戏专属设置", "清除遊戲專屬設定"},
    {"Delete game (remove from SD)", "Supprimer le jeu (retirer de la SD)", "Spiel löschen (von SD entfernen)", "Eliminar juego (quitar de la SD)", "Elimina gioco (rimuovi dalla SD)", "Eliminar jogo (remover do SD)", "删除游戏（从 SD 卡移除）", "刪除遊戲（從 SD 卡移除）"},
    {"Cover import failed", "Échec de l'importation de la jaquette", "Cover-Import fehlgeschlagen", "Error al importar la carátula", "Importazione copertina non riuscita", "Falha ao importar a capa", "封面导入失败", "封面匯入失敗"},
    {"Cover removal failed", "Échec de la suppression de la jaquette", "Cover-Entfernung fehlgeschlagen", "Error al quitar la carátula", "Rimozione copertina non riuscita", "Falha ao remover a capa", "封面移除失败", "封面移除失敗"},
    {"Create a user", "Créer un utilisateur", "Benutzer erstellen", "Crear un usuario", "Crea un utente", "Criar um utilizador", "创建用户", "建立使用者"},
    {"Manual module list", "Liste manuelle des modules", "Manuelle Modulliste", "Lista manual de módulos", "Elenco moduli manuale", "Lista manual de módulos", "手动模块列表", "手動模組清單"},
    {"None", "Aucun", "Keine", "Ninguno", "Nessuno", "Nenhum", "无", "無"},
    {"Press A to continue", "Appuyez sur A pour continuer", "Zum Fortfahren A drücken", "Pulsa A para continuar", "Premi A per continuare", "Prima A para continuar", "按 A 继续", "按 A 繼續"},
    {"Selected", "Sélectionné", "Ausgewählt", "Seleccionado", "Selezionato", "Selecionado", "已选择", "已選擇"},
    {"Touch anywhere to close", "Touchez l'écran pour fermer", "Zum Schließen tippen", "Toca en cualquier parte para cerrar", "Tocca lo schermo per chiudere", "Toque em qualquer lugar para fechar", "点按任意位置关闭", "點按任意位置關閉"},
    {"Users", "Utilisateurs", "Benutzer", "Usuarios", "Utenti", "Utilizadores", "用户", "使用者"},
    {"Vita3K creates a default user on first launch", "Vita3K crée un utilisateur par défaut au premier lancement", "Vita3K erstellt beim ersten Start einen Standardbenutzer", "Vita3K crea un usuario predeterminado al iniciarse por primera vez", "Vita3K crea un utente predefinito al primo avvio", "O Vita3K cria um utilizador predefinido no primeiro arranque", "Vita3K 在首次启动时创建默认用户", "Vita3K 在首次啟動時建立預設使用者"},
    {"no users yet", "aucun utilisateur", "noch keine Benutzer", "aún no hay usuarios", "nessun utente", "ainda sem utilizadores", "尚无用户", "尚無使用者"},
    {"Actions", "Actions", "Aktionen", "Acciones", "Azioni", "Ações", "操作", "動作"},
    {"Check Again", "Revérifier", "Erneut prüfen", "Buscar de nuevo", "Ricontrolla", "Verificar novamente", "重新检查", "重新檢查"},
    {"Clear all", "Tout effacer", "Alle löschen", "Borrar todo", "Cancella tutto", "Limpar tudo", "全部清除", "全部清除"},
    {"Delete", "Supprimer", "Löschen", "Eliminar", "Elimina", "Eliminar", "删除", "刪除"},
    {"Done", "Terminé", "Fertig", "Listo", "Fatto", "Concluído", "完成", "完成"},
    {"Download", "Télécharger", "Laden", "Descargar", "Scarica", "Transferir", "下载", "下載"},
    {"Edit", "Modifier", "Bearbeiten", "Editar", "Modifica", "Editar", "编辑", "編輯"},
    {"Filter", "Filtrer", "Filter", "Filtrar", "Filtra", "Filtrar", "筛选", "篩選"},
    {"Game Menu", "Menu du jeu", "Spielmenü", "Menú del juego", "Menu gioco", "Menu do jogo", "游戏菜单", "遊戲選單"},
    {"Help", "Aide", "Hilfe", "Ayuda", "Aiuto", "Ajuda", "帮助", "說明"},
    {"Install & Exit", "Installer et quitter", "Installieren & beenden", "Instalar y salir", "Installa ed esci", "Instalar e sair", "安装并退出", "安裝並退出"},
    {"Open", "Ouvrir", "Öffnen", "Abrir", "Apri", "Abrir", "打开", "開啟"},
    {"Open / Select", "Ouvrir / Sélectionner", "Öffnen / Auswählen", "Abrir / Seleccionar", "Apri / Seleziona", "Abrir / Selecionar", "打开 / 选择", "開啟 / 選擇"},
    {"Paste", "Coller", "Einfügen", "Pegar", "Incolla", "Colar", "粘贴", "貼上"},
    {"Quit", "Quitter", "Beenden", "Salir", "Esci", "Sair", "退出", "退出"},
    {"Rename", "Renommer", "Umbenennen", "Renombrar", "Rinomina", "Renomear", "重命名", "重新命名"},
    {"Select", "Sélectionner", "Auswählen", "Seleccionar", "Seleziona", "Selecionar", "选择", "選擇"},
    {"Sort", "Trier", "Sortieren", "Ordenar", "Ordina", "Ordenar", "排序", "排序"},
    {"Toggle", "Basculer", "Umschalten", "Alternar", "Cambia", "Alternar", "切换", "切換"},
    {"Use artwork", "Utiliser l'illustration", "Artwork verwenden", "Usar ilustración", "Usa immagine", "Usar imagem", "使用图片", "使用圖片"},
    {"Use icon", "Utiliser l'icône", "Icon verwenden", "Usar icono", "Usa icona", "Usar ícone", "使用图标", "使用圖示"},
    {"Choose an icon", "Choisir une icône", "Icon auswählen", "Elegir un icono", "Scegli un'icona", "Escolher um ícone", "选择图标", "選擇圖示"},
    {"Choose cover artwork", "Choisir une jaquette", "Cover auswählen", "Elegir carátula", "Scegli la copertina", "Escolher a imagem da capa", "选择封面图", "選擇封面圖"},
    {"Choose import storage", "Choisir le stockage d'importation", "Importspeicher auswählen", "Elegir almacenamiento", "Scegli la memoria di importazione", "Escolher o armazenamento de importação", "选择导入存储", "選擇匯入儲存"},
    {"Creating HOME shortcut", "Création du raccourci HOME", "HOME-Verknüpfung wird erstellt", "Creando acceso directo HOME", "Creazione collegamento HOME", "A criar o atalho HOME", "正在创建 HOME 快捷方式", "正在建立 HOME 捷徑"},
    {"File transfer", "Transfert de fichiers", "Dateiübertragung", "Transferencia de archivos", "Trasferimento file", "Transferência de ficheiros", "文件传输", "檔案傳輸"},
    {"Firmware setup", "Configuration du micrologiciel", "Firmware-Einrichtung", "Configuración del firmware", "Configurazione firmware", "Configuração do firmware", "固件设置", "韌體設定"},
    {"Game menu", "Menu du jeu", "Spielmenü", "Menú del juego", "Menu gioco", "Menu do jogo", "游戏菜单", "遊戲選單"},
    {"Vita3K-nx Update", "Mise à jour de Vita3K-nx", "Vita3K-nx-Update", "Actualización de Vita3K-nx", "Aggiornamento Vita3K-nx", "Atualização do Vita3K-nx", "Vita3K-nx 更新", "Vita3K-nx 更新"},
    {"Chooses which decrypted firmware modules are loaded natively instead of being emulated. It needs Modules mode set to something other than Automatic, and installed firmware. Picking nothing in Manual mode loads no modules at all, which is usually worse than Automatic.", "Choisit les modules de micrologiciel déchiffrés chargés nativement au lieu d'être émulés. Nécessite un Mode des modules autre qu'Automatique et un micrologiciel installé. Ne rien choisir en mode Manuel ne charge aucun module, ce qui est généralement pire qu'Automatique.", "Legt fest, welche entschlüsselten Firmware-Module nativ geladen statt emuliert werden. Erfordert einen Modulmodus außer Automatisch und installierte Firmware. Wird im Modus Manuell nichts gewählt, werden gar keine Module geladen, was meist schlechter ist als Automatisch.", "Elige qué módulos de firmware descifrados se cargan de forma nativa en vez de emularse. Requiere que Modo de módulos no sea Automático y tener firmware instalado. No elegir nada en modo Manual no carga ningún módulo, lo que suele ser peor que Automático.", "Sceglie quali moduli firmware decriptati vengono caricati in modo nativo anziché emulati. Richiede Modalità moduli su un valore diverso da Automatico e il firmware installato. In modalità Manuale, non selezionare nulla non carica alcun modulo, cosa di solito peggiore di Automatico.", "Escolhe que módulos de firmware desencriptados são carregados nativamente em vez de emulados. Requer o Modo de módulos diferente de Automático e firmware instalado. Não escolher nada no modo Manual não carrega qualquer módulo, o que costuma ser pior do que Automático.", "选择哪些已解密的固件模块以原生方式加载，而不是被模拟。需要将「模块模式」设置为自动以外的选项，并已安装固件。在手动模式下不选择任何模块则完全不会加载模块，这通常比自动模式更差。", "選擇哪些已解密的韌體模組以原生方式載入，而不是被模擬。需要將「模組模式」設定為自動以外的選項，並已安裝韌體。在手動模式下不選擇任何模組則完全不會載入模組，這通常比自動模式更差。"},
    {"Fills the whole screen instead of preserving the Vita 16:9.4 aspect. The image is distorted, and touch coordinates follow the stretched area.", "Remplit tout l'écran au lieu de conserver le rapport 16:9.4 de la Vita. L'image est déformée et les coordonnées tactiles suivent la zone étirée.", "Füllt den gesamten Bildschirm, statt das Vita-Seitenverhältnis 16:9.4 beizubehalten. Das Bild wird verzerrt, und die Touch-Koordinaten folgen dem gestreckten Bereich.", "Llena toda la pantalla en vez de conservar la relación 16:9.4 de Vita. La imagen se distorsiona y las coordenadas táctiles siguen el área estirada.", "Riempie tutto lo schermo invece di mantenere le proporzioni 16:9.4 della Vita. L'immagine risulta distorta e le coordinate touch seguono l'area allungata.", "Preenche todo o ecrã em vez de manter a proporção 16:9.4 da Vita. A imagem fica distorcida e as coordenadas táteis seguem a área esticada.", "填满整个屏幕，而不是保持 Vita 的 16:9.4 纵横比。图像会变形，触摸坐标也会跟随拉伸后的区域。", "填滿整個螢幕，而不是保持 Vita 的 16:9.4 長寬比。影像會變形，觸控座標也會跟隨拉伸後的區域。"},
    {"Shows or hides the coloured compatibility dot in the bottom-left corner of each cover. Download the database from Library & storage first.", "Affiche ou masque la pastille de compatibilité colorée dans le coin inférieur gauche de chaque jaquette. Téléchargez d'abord la base de compatibilité depuis Bibliothèque et stockage.", "Blendet den farbigen Kompatibilitätspunkt unten links auf jedem Cover ein oder aus. Lade die Datenbank zuvor unter Bibliothek & Speicher herunter.", "Muestra u oculta el punto de compatibilidad de color en la esquina inferior izquierda de cada carátula. Descarga antes la base de datos desde Biblioteca y almacenamiento.", "Mostra o nasconde il pallino colorato di compatibilità nell'angolo inferiore sinistro di ogni copertina. Scarica prima il database da Libreria e archiviazione.", "Mostra ou oculta o ponto colorido de compatibilidade no canto inferior esquerdo de cada capa. Transfira primeiro a base de dados em Biblioteca e armazenamento.", "显示或隐藏每个封面左下角的彩色兼容性圆点。请先从「游戏库与存储」下载数据库。", "顯示或隱藏每個封面左下角的彩色相容性圓點。請先從「遊戲庫與儲存」下載資料庫。"},
    {"User profiles", "Profils utilisateur", "Benutzerprofile", "Perfiles de usuario", "Profili utente", "Perfis de utilizador", "用户档案", "使用者設定檔"},
    {"Vita users", "Utilisateurs Vita", "Vita-Benutzer", "Usuarios de Vita", "Utenti Vita", "Utilizadores Vita", "Vita 用户", "Vita 使用者"},
    {"Show compatibility badges", "Afficher les indicateurs de compatibilité", "Kompatibilitätsmarkierungen anzeigen", "Mostrar indicadores de compatibilidad", "Mostra indicatori di compatibilità", "Mostrar indicadores de compatibilidade", "显示兼容性徽标", "顯示相容性徽章"},
    {"Stretch to screen", "Étirer à l'écran", "Auf Bildschirm strecken", "Estirar a la pantalla", "Estendi a tutto schermo", "Esticar para o ecrã", "拉伸至全屏", "拉伸至全螢幕"},
    {"Controller", "Manette", "Controller", "Mando", "Controller", "Comando", "手柄", "手把"},
    {"Emulation", "Émulation", "Emulation", "Emulación", "Emulazione", "Emulação", "模拟", "模擬"},
    {"Frame Generation", "Génération d'images", "Frame-Generierung", "Generación de fotogramas", "Generazione fotogrammi", "Geração de fotogramas", "帧生成", "幀生成"},
    {"GPU / Graphics", "GPU / Graphismes", "GPU / Grafik", "GPU / Gráficos", "GPU / Grafica", "GPU / Gráficos", "GPU / 图形", "GPU / 圖形"},
    {"A SteamGridDB API key is required", "Une clé API SteamGridDB est requise", "Ein SteamGridDB-API-Schlüssel ist erforderlich", "Se necesita una clave API de SteamGridDB", "È richiesta una chiave API SteamGridDB", "É necessária uma chave API SteamGridDB", "需要 SteamGridDB API 密钥", "需要 SteamGridDB API 金鑰"},
    {"All covers already downloaded", "Toutes les jaquettes sont déjà téléchargées", "Alle Cover bereits heruntergeladen", "Todas las carátulas ya están descargadas", "Tutte le copertine sono già scaricate", "Todas as capas já foram transferidas", "所有封面均已下载", "所有封面均已下載"},
    {"Cache cleared", "Cache vidé", "Cache geleert", "Caché borrada", "Cache svuotata", "Cache limpa", "缓存已清除", "快取已清除"},
    {"Copied to clipboard", "Copié dans le presse-papiers", "In die Zwischenablage kopiert", "Copiado al portapapeles", "Copiato negli appunti", "Copiado para a área de transferência", "已复制到剪贴板", "已複製到剪貼簿"},
    {"Could not change launcher orientation", "Impossible de changer l'orientation du lanceur", "Launcher-Ausrichtung konnte nicht geändert werden", "No se pudo cambiar la orientación del lanzador", "Impossibile cambiare l'orientamento del launcher", "Não foi possível alterar a orientação do launcher", "无法更改启动器方向", "無法變更啟動器方向"},
    {"Could not create the user", "Impossible de créer l'utilisateur", "Benutzer konnte nicht erstellt werden", "No se pudo crear el usuario", "Impossibile creare l'utente", "Não foi possível criar o utilizador", "无法创建用户", "無法建立使用者"},
    {"Could not reach the compatibility database", "Impossible de joindre la base de compatibilité", "Kompatibilitätsdatenbank nicht erreichbar", "No se pudo acceder a la base de datos de compatibilidad", "Impossibile raggiungere il database di compatibilità", "Não foi possível aceder à base de dados de compatibilidade", "无法访问兼容性数据库", "無法存取相容性資料庫"},
    {"Could not rename the user", "Impossible de renommer l'utilisateur", "Benutzer konnte nicht umbenannt werden", "No se pudo renombrar el usuario", "Impossibile rinominare l'utente", "Não foi possível renomear o utilizador", "无法重命名用户", "無法重新命名使用者"},
    {"Could not save the compatibility database", "Impossible d'enregistrer la base de compatibilité", "Kompatibilitätsdatenbank konnte nicht gespeichert werden", "No se pudo guardar la base de datos de compatibilidad", "Impossibile salvare il database di compatibilità", "Não foi possível guardar a base de dados de compatibilidade", "无法保存兼容性数据库", "無法儲存相容性資料庫"},
    {"Cover download failed", "Échec du téléchargement de la jaquette", "Cover-Download fehlgeschlagen", "Error al descargar la carátula", "Download della copertina non riuscito", "Falha ao transferir a capa", "封面下载失败", "封面下載失敗"},
    {"Cover downloaded", "Jaquette téléchargée", "Cover heruntergeladen", "Carátula descargada", "Copertina scaricata", "Capa transferida", "封面已下载", "封面已下載"},
    {"Firmware downloaded - installing...", "Micrologiciel téléchargé - installation...", "Firmware heruntergeladen - wird installiert...", "Firmware descargado - instalando...", "Firmware scaricato - installazione...", "Firmware transferido - a instalar...", "固件已下载 - 正在安装...", "韌體已下載 - 正在安裝..."},
    {"Game deleted", "Jeu supprimé", "Spiel gelöscht", "Juego eliminado", "Gioco eliminato", "Jogo eliminado", "游戏已删除", "遊戲已刪除"},
    {"HOME shortcut installed", "Raccourci HOME installé", "HOME-Verknüpfung installiert", "Acceso directo HOME instalado", "Collegamento HOME installato", "Atalho HOME instalado", "HOME 快捷方式已安装", "HOME 捷徑已安裝"},
    {"Installing firmware from local files...", "Installation du micrologiciel depuis des fichiers locaux...", "Firmware wird aus lokalen Dateien installiert...", "Instalando firmware desde archivos locales...", "Installazione firmware da file locali...", "A instalar o firmware a partir de ficheiros locais...", "正在从本地文件安装固件...", "正在從本機檔案安裝韌體..."},
    {"Maximum of 24 pinned folders", "24 dossiers épinglés au maximum", "Maximal 24 angeheftete Ordner", "Máximo de 24 carpetas ancladas", "Massimo 24 cartelle fissate", "Máximo de 24 pastas fixadas", "最多 24 个固定文件夹", "最多 24 個釘選資料夾"},
    {"Maximum of 8 SMB shares", "8 partages SMB au maximum", "Maximal 8 SMB-Freigaben", "Máximo de 8 recursos SMB", "Massimo 8 condivisioni SMB", "Máximo de 8 partilhas SMB", "最多 8 个 SMB 共享", "最多 8 個 SMB 共用"},
    {"Move complete", "Déplacement terminé", "Verschieben abgeschlossen", "Movimiento completado", "Spostamento completato", "Movimentação concluída", "移动完成", "移動完成"},
    {"Move queued", "Déplacement en file d'attente", "Verschieben eingereiht", "Movimiento en cola", "Spostamento in coda", "Movimentação em fila", "移动已排队", "移動已排入佇列"},
    {"No free user slot is available", "Aucun emplacement utilisateur libre", "Kein freier Benutzerplatz verfügbar", "No hay ningún espacio de usuario libre", "Nessuno slot utente disponibile", "Não há espaços de utilizador livres", "没有可用的用户位", "沒有可用的使用者欄位"},
    {"No icon found - add a SteamGridDB key or download a cover first", "Aucune icône trouvée - ajoutez une clé SteamGridDB ou téléchargez une jaquette", "Kein Icon gefunden - füge zuerst einen SteamGridDB-Schlüssel hinzu oder lade ein Cover herunter", "No se encontró ningún icono - añade una clave de SteamGridDB o descarga antes una carátula", "Nessuna icona trovata - aggiungi una chiave SteamGridDB o scarica prima una copertina", "Nenhum ícone encontrado - adicione uma chave SteamGridDB ou transfira primeiro uma capa", "未找到图标 - 请先添加 SteamGridDB 密钥或下载封面", "未找到圖示 - 請先新增 SteamGridDB 金鑰或下載封面"},
    {"No network connection is available", "Aucune connexion réseau disponible", "Keine Netzwerkverbindung verfügbar", "No hay conexión de red disponible", "Nessuna connessione di rete disponibile", "Não há ligação de rede disponível", "没有可用的网络连接", "沒有可用的網路連線"},
    {"Per-game settings cleared", "Paramètres par jeu effacés", "Spielspezifische Einstellungen gelöscht", "Ajustes por juego borrados", "Impostazioni per gioco cancellate", "Definições por jogo limpas", "已清除游戏专属设置", "已清除遊戲專屬設定"},
    {"Pick an icon first", "Choisissez d'abord une icône", "Wähle zuerst ein Icon", "Elige antes un icono", "Scegli prima un'icona", "Escolha primeiro um ícone", "请先选择图标", "請先選擇圖示"},
    {"Renamed", "Renommé", "Umbenannt", "Renombrado", "Rinominato", "Renomeado", "已重命名", "已重新命名"},
    {"The compatibility database is already up to date", "La base de compatibilité est déjà à jour", "Die Kompatibilitätsdatenbank ist bereits aktuell", "La base de datos de compatibilidad ya está actualizada", "Il database di compatibilità è già aggiornato", "A base de dados de compatibilidade já está atualizada", "兼容性数据库已是最新", "相容性資料庫已是最新"},
    {"The downloaded compatibility database was unreadable", "La base de compatibilité téléchargée est illisible", "Die heruntergeladene Kompatibilitätsdatenbank war unlesbar", "No se pudo leer la base de datos de compatibilidad descargada", "Il database di compatibilità scaricato è illeggibile", "A base de dados de compatibilidade transferida era ilegível", "下载的兼容性数据库无法读取", "下載的相容性資料庫無法讀取"},
    {"Transfer cancelled", "Transfert annulé", "Übertragung abgebrochen", "Transferencia cancelada", "Trasferimento annullato", "Transferência cancelada", "传输已取消", "傳輸已取消"},
    {"Transfer complete", "Transfert terminé", "Übertragung abgeschlossen", "Transferencia completada", "Trasferimento completato", "Transferência concluída", "传输完成", "傳輸完成"},
    {"USB drive can now be removed", "Le périphérique USB peut être retiré", "USB-Laufwerk kann jetzt entfernt werden", "Ya se puede retirar la unidad USB", "Ora puoi rimuovere l'unità USB", "A unidade USB já pode ser removida", "现在可以移除 USB 设备", "現在可以移除 USB 裝置"},
    {"User created", "Utilisateur créé", "Benutzer erstellt", "Usuario creado", "Utente creato", "Utilizador criado", "用户已创建", "使用者已建立"},
    {"User deleted", "Utilisateur supprimé", "Benutzer gelöscht", "Usuario eliminado", "Utente eliminato", "Utilizador eliminado", "用户已删除", "使用者已刪除"},
    {"User renamed", "Utilisateur renommé", "Benutzer umbenannt", "Usuario renombrado", "Utente rinominato", "Utilizador renomeado", "用户已重命名", "使用者已重新命名"},
    {"User selected", "Utilisateur sélectionné", "Benutzer ausgewählt", "Usuario seleccionado", "Utente selezionato", "Utilizador selecionado", "已选择用户", "已選擇使用者"},
    {"A HOME forwarder needs sigpatches on your CFW.", "Un forwarder HOME nécessite des sigpatches sur votre CFW.", "Ein HOME-Forwarder benötigt sigpatches auf deiner CFW.", "Un forwarder HOME necesita sigpatches en tu CFW.", "Un forwarder HOME richiede i sigpatches sul tuo CFW.", "Um forwarder HOME precisa de sigpatches no seu CFW.", "HOME 转发器需要你的 CFW 支持 sigpatches。", "HOME 轉發器需要你的 CFW 支援 sigpatches。"},
    {"All of this user's save data and trophies are deleted.", "Toutes les sauvegardes et tous les trophées de cet utilisateur sont supprimés.", "Alle Spielstände und Trophäen dieses Benutzers werden gelöscht.", "Se eliminan todos los datos de guardado y trofeos de este usuario.", "Tutti i dati di salvataggio e i trofei di questo utente vengono eliminati.", "Todos os dados guardados e troféus deste utilizador são eliminados.", "此用户的所有存档数据和奖杯都会被删除。", "此使用者的所有存檔資料和獎盃都會被刪除。"},
    {"Choose another destination or rename the folder first.", "Choisissez une autre destination ou renommez d'abord le dossier.", "Wähle ein anderes Ziel oder benenne den Ordner zuerst um.", "Elige otro destino o renombra antes la carpeta.", "Scegli un'altra destinazione o rinomina prima la cartella.", "Escolha outro destino ou renomeie primeiro a pasta.", "请选择其他目标位置，或先重命名该文件夹。", "請選擇其他目標位置，或先重新命名該資料夾。"},
    {"Close files using this drive before ejecting.", "Fermez les fichiers utilisant ce périphérique avant de l'éjecter.", "Schließe Dateien auf diesem Laufwerk vor dem Auswerfen.", "Cierra los archivos que usen esta unidad antes de expulsarla.", "Chiudi i file che usano questa unità prima di espellerla.", "Feche os ficheiros que usam esta unidade antes de ejetar.", "弹出前请关闭使用此设备的所有文件。", "退出前請關閉使用此裝置的所有檔案。"},
    {"Deletes compiled shaders and shader logs.", "Supprime les shaders compilés et leurs journaux.", "Löscht kompilierte Shader und Shader-Logs.", "Elimina los shaders compilados y sus registros.", "Elimina gli shader compilati e i relativi log.", "Elimina os shaders compilados e os registos de shaders.", "删除已编译的着色器和着色器日志。", "刪除已編譯的著色器和著色器日誌。"},
    {"Games and files are not deleted.", "Les jeux et les fichiers ne sont pas supprimés.", "Spiele und Dateien werden nicht gelöscht.", "Los juegos y archivos no se eliminan.", "I giochi e i file non vengono eliminati.", "Os jogos e os ficheiros não são eliminados.", "游戏和文件不会被删除。", "遊戲和檔案不會被刪除。"},
    {"Import the companion license with the selected package?", "Importer la licence associée avec le paquet sélectionné ?", "Die zugehörige Lizenz mit dem gewählten Paket importieren?", "¿Importar la licencia asociada con el paquete seleccionado?", "Importare la licenza associata con il pacchetto selezionato?", "Importar a licença associada com o pacote selecionado?", "要随所选安装包一起导入配套许可证吗？", "要隨所選安裝包一起匯入配套授權嗎？"},
    {"Installed games and their files are not touched.", "Les jeux installés et leurs fichiers ne sont pas modifiés.", "Installierte Spiele und ihre Dateien bleiben unverändert.", "Los juegos instalados y sus archivos no se modifican.", "I giochi installati e i loro file non vengono modificati.", "Os jogos instalados e os seus ficheiros não são alterados.", "已安装的游戏及其文件不会受影响。", "已安裝的遊戲及其檔案不會受到影響。"},
    {"Make sure the console is online and DNS is reachable.", "Vérifiez que la console est en ligne et que le DNS est joignable.", "Stelle sicher, dass die Konsole online und DNS erreichbar ist.", "Asegúrate de que la consola esté en línea y el DNS sea accesible.", "Verifica che la console sia online e che il DNS sia raggiungibile.", "Certifique-se de que a consola está online e o DNS acessível.", "请确保主机在线且 DNS 可访问。", "請確保主機線上且 DNS 可存取。"},
    {"No files on the server will be deleted.", "Aucun fichier du serveur ne sera supprimé.", "Auf dem Server werden keine Dateien gelöscht.", "No se eliminará ningún archivo del servidor.", "Nessun file sul server verrà eliminato.", "Nenhum ficheiro no servidor será eliminado.", "服务器上的文件不会被删除。", "伺服器上的檔案不會被刪除。"},
    {"No new installer job was started.", "Aucune nouvelle tâche d'installation n'a été lancée.", "Es wurde kein neuer Installationsvorgang gestartet.", "No se inició ninguna tarea de instalación.", "Nessuna nuova operazione di installazione è stata avviata.", "Não foi iniciada nenhuma nova tarefa de instalação.", "没有启动新的安装任务。", "沒有啟動新的安裝工作。"},
    {"Save data and game files are not changed.", "Les sauvegardes et les fichiers de jeu ne sont pas modifiés.", "Spielstände und Spieldateien werden nicht geändert.", "Los datos de guardado y los archivos del juego no se modifican.", "I dati di salvataggio e i file di gioco non vengono modificati.", "Os dados guardados e os ficheiros do jogo não são alterados.", "存档数据和游戏文件不会被更改。", "存檔資料和遊戲檔案不會被變更。"},
    {"The device may be disconnected.", "Le périphérique est peut-être déconnecté.", "Das Gerät ist möglicherweise getrennt.", "Puede que el dispositivo esté desconectado.", "Il dispositivo potrebbe essere scollegato.", "O dispositivo pode estar desligado.", "设备可能已断开连接。", "裝置可能已斷開連線。"},
    {"The downloaded or imported cover will be deleted.", "La jaquette téléchargée ou importée sera supprimée.", "Das heruntergeladene oder importierte Cover wird gelöscht.", "Se eliminará la carátula descargada o importada.", "La copertina scaricata o importata verrà eliminata.", "A capa transferida ou importada será eliminada.", "已下载或导入的封面将被删除。", "已下載或匯入的封面將被刪除。"},
    {"The existing file will be replaced.", "Le fichier existant sera remplacé.", "Die vorhandene Datei wird ersetzt.", "Se sustituirá el archivo existente.", "Il file esistente verrà sostituito.", "O ficheiro existente será substituído.", "现有文件将被替换。", "現有檔案將被取代。"},
    {"The file transfer could not be completed.", "Le transfert de fichiers n'a pas pu être terminé.", "Die Dateiübertragung konnte nicht abgeschlossen werden.", "No se pudo completar la transferencia de archivos.", "Non è stato possibile completare il trasferimento dei file.", "Não foi possível concluir a transferência de ficheiros.", "文件传输无法完成。", "檔案傳輸無法完成。"},
    {"The installed launcher was left unchanged where possible.", "Le lanceur installé a été laissé inchangé dans la mesure du possible.", "Der installierte Launcher blieb nach Möglichkeit unverändert.", "El lanzador instalado se dejó sin cambios cuando fue posible.", "Il launcher installato è rimasto invariato dove possibile.", "O launcher instalado foi mantido inalterado sempre que possível.", "已安装的启动器在可能的情况下保持不变。", "已安裝的啟動器在可能的情況下保持不變。"},
    {"The launcher will use the game's embedded artwork when available.", "Le lanceur utilisera l'illustration intégrée du jeu si elle est disponible.", "Der Launcher verwendet das eingebettete Artwork des Spiels, sofern vorhanden.", "El lanzador usará la ilustración incluida en el juego cuando esté disponible.", "Il launcher userà l'immagine integrata del gioco, se disponibile.", "O launcher usará a imagem incorporada do jogo quando disponível.", "启动器将尽可能使用游戏自带的封面图。", "啟動器將盡可能使用遊戲內建的封面圖。"},
    {"The selected folder is outside this storage root.", "Le dossier sélectionné est hors de la racine de ce stockage.", "Der gewählte Ordner liegt außerhalb dieses Speicherstammverzeichnisses.", "La carpeta seleccionada está fuera de la raíz de este almacenamiento.", "La cartella selezionata è fuori dalla radice di questa memoria.", "A pasta selecionada está fora da raiz deste armazenamento.", "所选文件夹位于此存储根目录之外。", "所選資料夾位於此儲存根目錄之外。"},
    {"This cannot be undone.", "Cette action est irréversible.", "Dies kann nicht rückgängig gemacht werden.", "Esta acción no se puede deshacer.", "L'operazione non può essere annullata.", "Esta ação não pode ser anulada.", "此操作无法撤销。", "此操作無法復原。"},
    {"This permanently deletes the game from the SD card.", "Cela supprime définitivement le jeu de la carte SD.", "Dies löscht das Spiel dauerhaft von der SD-Karte.", "Esto elimina el juego de la tarjeta SD de forma permanente.", "Il gioco viene eliminato definitivamente dalla scheda SD.", "Isto elimina permanentemente o jogo do cartão SD.", "这将从 SD 卡永久删除该游戏。", "這將從 SD 卡永久刪除該遊戲。"},
    {"Try again, or copy the PUP files from SD, USB, or SMB instead.", "Réessayez ou copiez plutôt les fichiers PUP depuis SD, USB ou SMB.", "Versuche es erneut oder kopiere die PUP-Dateien stattdessen von SD, USB oder SMB.", "Reinténtalo o copia los archivos PUP desde SD, USB o SMB.", "Riprova oppure copia i file PUP da SD, USB o SMB.", "Tente novamente ou copie os ficheiros PUP de SD, USB ou SMB.", "请重试，或改为从 SD、USB 或 SMB 复制 PUP 文件。", "請重試，或改為從 SD、USB 或 SMB 複製 PUP 檔案。"},
    {"Unknown error", "Erreur inconnue", "Unbekannter Fehler", "Error desconocido", "Errore sconosciuto", "Erro desconhecido", "未知错误", "未知錯誤"},
    {"Importing", "Importation", "Importieren", "Importando", "Importazione", "A importar", "正在导入", "正在匯入"},
    {"Preparing import...", "Préparation de l'importation...", "Import wird vorbereitet...", "Preparando la importación...", "Preparazione dell'importazione...", "A preparar a importação...", "正在准备导入...", "正在準備匯入..."},
    {"Cancelling import...", "Annulation de l'importation...", "Import wird abgebrochen...", "Cancelando la importación...", "Annullamento dell'importazione...", "A cancelar a importação...", "正在取消导入...", "正在取消匯入..."},
    {"Installed", "Installé", "Installiert", "Instalado", "Installato", "Instalado", "已安装", "已安裝"},
    {"Missing", "Manquant", "Fehlt", "Ausente", "Mancante", "Em falta", "缺失", "遺失"},
    {"Configured", "Configuré", "Konfiguriert", "Configurado", "Configurato", "Configurado", "已配置", "已設定"},
    {"Not configured", "Non configuré", "Nicht konfiguriert", "Sin configurar", "Non configurato", "Não configurado", "未配置", "未設定"},
    {"Setting", "Paramètre", "Einstellung", "Ajuste", "Impostazione", "Definição", "设置", "設定"},
    {"Setting help", "Aide sur le paramètre", "Hilfe zur Einstellung", "Ayuda del ajuste", "Guida all'impostazione", "Ajuda da definição", "设置帮助", "設定說明"},
    {"The selected cover could not be imported safely.", "La jaquette sélectionnée n'a pas pu être importée en toute sécurité.", "Das ausgewählte Cover konnte nicht sicher importiert werden.", "No se pudo importar la carátula seleccionada de forma segura.", "Non è stato possibile importare la copertina selezionata in modo sicuro.", "Não foi possível importar a capa selecionada em segurança.", "所选封面无法安全导入。", "所選封面無法安全匯入。"},
    {"Allowed CPU cores", "", "", "", "", "", "可用 CPU 核心", "可用 CPU 核心"},
    {"How many of the console's four CPU cores this session may use. Four is correct: the emulator keeps vblank, audio and rendering on the fourth core so all three others stay free for the emulated Vita. Three means the launcher was started without a shortcut, and that work has to share the game's cores. Create a shortcut from the installer to get the fourth.", "", "", "", "", "", "显示本次运行可使用的主机四个 CPU 核心中的数量。4 个为正常状态：模拟器会把垂直同步、音频和渲染放在第四个核心，让其余三个核心专用于模拟 Vita。3 个表示启动器未通过快捷方式启动，这些任务必须与游戏共享核心。请从安装程序创建快捷方式以使用第四个核心。", "顯示本次執行可使用的主機四個 CPU 核心中的數量。4 個為正常狀態：模擬器會把垂直同步、音訊和渲染放在第四個核心，讓其餘三個核心專用於模擬 Vita。3 個表示啟動器未透過捷徑啟動，這些工作必須與遊戲共享核心。請從安裝程式建立捷徑以使用第四個核心。"},
    {"File loading delay (ms)", "", "", "", "", "", "文件加载延迟（毫秒）", "檔案載入延遲（毫秒）"},
    {"Adds a delay to file reads for games that depend on storage timing. 0 ms disables it.", "", "", "", "", "", "为依赖存储时序的游戏延迟文件读取。0 毫秒表示禁用。", "為依賴儲存時序的遊戲延遲檔案讀取。0 毫秒表示停用。"},
    {"Full-precision shaders", "", "", "", "", "", "全精度着色器", "全精度著色器"},
    {"Uses full precision for Vulkan shader inputs and outputs. It may fix rendering errors at a performance cost.", "", "", "", "", "", "对 Vulkan 着色器输入和输出使用完整精度。可能修复渲染错误，但会降低性能。", "對 Vulkan 著色器輸入和輸出使用完整精度。可能修正渲染錯誤，但會降低效能。"},
    {"FSR sharpness", "", "", "", "", "", "FSR 锐度", "FSR 銳利度"},
    {"Adjusts FSR sharpening: 0.0 is strongest, 2.0 is softer. The default is 0.2. Requires Vulkan and the FSR screen filter.", "", "", "", "", "", "调整 FSR 锐化强度：0.0 最强，2.0 较柔和。默认值为 0.2。需要 Vulkan 和 FSR 屏幕滤镜。", "調整 FSR 銳化強度：0.0 最強，2.0 較柔和。預設值為 0.2。需要 Vulkan 和 FSR 螢幕濾鏡。"},
    {"Direct SPIR-V", "", "", "", "", "", "直接使用 SPIR-V", "直接使用 SPIR-V"},
    {"Uses SPIR-V directly on Zink, with GLSL fallback if unsupported. Native NVC0 disables this extension pending testing.", "", "", "", "", "", "在 Zink 上直接使用 SPIR-V；不支持时回退到 GLSL。原生 NVC0 在完成测试前会禁用此扩展。", "在 Zink 上直接使用 SPIR-V；不支援時會退回 GLSL。原生 NVC0 在完成測試前會停用此擴充功能。"},
    {"Debug shader dumps", "", "", "", "", "", "调试着色器转储", "偵錯著色器傾印"},
    {"Saves guest shaders and cached shader code for graphics troubleshooting. Increases SD-card I/O.", "", "", "", "", "", "保存游戏着色器和缓存的着色器代码，用于排查图形问题。会增加 SD 卡读写。", "儲存遊戲著色器和快取的著色器程式碼，以便排查圖形問題。會增加 SD 卡存取。"},
    {"External host", "", "", "", "", "", "外部主机", "外部主機"},
    {"Selects how Vulkan sees Vita GPU memory. External host maps the Vita's own memory straight to the GPU (fastest, default). Double buffer keeps a checked copy instead. Disabled turns mapping off entirely, which some games need.", "", "", "", "", "", "选择 Vulkan 访问 Vita GPU 内存的方式。“外部主机”将 Vita 自身内存直接映射到 GPU（速度最快，默认）。“双缓冲”改用经过检查的副本。“禁用”会完全关闭内存映射，部分游戏需要此设置。", "選擇 Vulkan 存取 Vita GPU 記憶體的方式。「外部主機」會將 Vita 本身的記憶體直接對應到 GPU（速度最快，預設）。「雙重緩衝」改用經過檢查的副本。「停用」會完全關閉記憶體對應，部分遊戲需要此設定。"},
    {"Rear touch buttons", "", "", "", "", "", "背面触控按键", "背面觸控按鍵"},
    {"Puts L2, R2, L3 and R3 on the quadrants of the Vita rear touch panel, where most games expect them: ZL and ZR press the top two, the stick clicks the bottom two. Takes over those four buttons, so the rear touch modifier above is ignored.", "", "", "", "", "", "将 L2、R2、L3 和 R3 映射到 Vita 背面触控板的四个区域，符合大多数游戏的预期：ZL 和 ZR 触发上方两个区域，摇杆按键触发下方两个区域。此设置会占用这四个按键，因此会忽略上方的背面触控修饰键。", "將 L2、R2、L3 和 R3 對應到 Vita 背面觸控板的四個區域，符合大多數遊戲的預期：ZL 和 ZR 觸發上方兩個區域，搖桿按鍵觸發下方兩個區域。此設定會占用這四個按鍵，因此會忽略上方的背面觸控輔助鍵。"},
    {"Front and rear together", "", "", "", "", "", "正面和背面同时", "正面和背面同時"},
    {"Chooses the shoulder-button modifier used with the touchscreen to emulate the Vita rear touch panel. \"Front and rear together\" uses no modifier and reports every touch on both panels at once, for games that ask for the two to be pressed together. Like the other choices here it needs \"Rear touch buttons\" below turned off.", "", "", "", "", "", "选择配合触摸屏使用的肩键，以模拟 Vita 背面触控。\"正面和背面同时\"无需修饰键，会将每次触摸同时报告给两个面板，适用于要求同时按下两者的游戏。与此处其他选项一样，需要关闭下方的\"背面触控按键\"。", "選擇配合觸控螢幕使用的肩鍵，以模擬 Vita 背面觸控。\"正面和背面同時\"不需要輔助鍵，會將每次觸控同時回報給兩個面板，適用於要求同時按下兩者的遊戲。與此處其他選項一樣，需要關閉下方的\"背面觸控按鍵\"。"},
    {"user", "utilisateur", "Benutzer", "usuario", "utente", "utilizador", "用户", "使用者"},
    {"users", "utilisateurs", "Benutzer", "usuarios", "utenti", "utilizadores", "用户", "使用者"},
};

std::unordered_map<std::string, std::string> s_translations;
std::string s_preference = "system";
std::string s_code = "en";

std::string ResolveSystemLanguage()
{
  u64 current = 0;
  if (R_FAILED(setInitialize()))
    return "en";
  const Result get_result = setGetSystemLanguage(&current);
  std::string result = "en";
  if (R_SUCCEEDED(get_result))
  {
    const struct { ::SetLanguage language; const char* code; } candidates[] = {
        {SetLanguage_FR, "fr"}, {SetLanguage_FRCA, "fr"}, {SetLanguage_DE, "de"},
        {SetLanguage_ES, "es"}, {SetLanguage_ES419, "es"}, {SetLanguage_IT, "it"},
        {SetLanguage_PT, "pt"}, {SetLanguage_PTBR, "pt"},
        {SetLanguage_ZHCN, "zh-Hans"}, {SetLanguage_ZHHANS, "zh-Hans"},
        {SetLanguage_ZHTW, "zh-Hant"}, {SetLanguage_ZHHANT, "zh-Hant"},
    };
    for (const auto& candidate : candidates)
    {
      u64 code = 0;
      if (R_SUCCEEDED(setMakeLanguageCode(candidate.language, &code)) && code == current)
      {
        result = candidate.code;
        break;
      }
    }
  }
  setExit();
  return result;
}
}

void SetLanguage(std::string preference)
{
  if (FindLanguage(preference) < 0)
    preference = "system";
  s_preference = std::move(preference);
  s_code = s_preference == "system" ? ResolveSystemLanguage() : s_preference;
  s_translations.clear();
  int column = 0;
  if (s_code == "fr") column = 1;
  else if (s_code == "de") column = 2;
  else if (s_code == "es") column = 3;
  else if (s_code == "it") column = 4;
  else if (s_code == "pt") column = 5;
  else if (s_code == "zh-Hans") column = 6;
  else if (s_code == "zh-Hant") column = 7;
  if (!column)
    return;
  for (const Entry& entry : ENTRIES)
  {
    const char* translated[] = {entry.source, entry.fr, entry.de, entry.es, entry.it, entry.pt, entry.zh_Hans, entry.zh_Hant};
    if (translated[column] && *translated[column])
      s_translations.emplace(entry.source, translated[column]);
  }
}

std::string_view Translate(std::string_view source)
{
  const auto found = s_translations.find(std::string(source));
  return found == s_translations.end() ? source : std::string_view(found->second);
}

const std::string& Preference()
{
  return s_preference;
}

std::string_view CurrentLanguage(){
  return s_code;
}

std::string DisplayName()
{
  const int index = FindLanguage(s_preference);
  if (index <= 0)
  {
    const int resolved = FindLanguage(s_code);
    return std::string(Translate("System")) + " (" +
           (resolved >= 0 ? LANGUAGE_LIST[resolved].name : "English") + ")";
  }
  return LANGUAGE_LIST[index].name;
}

const std::vector<Language>& Languages()
{
  static const std::vector<Language> result(LANGUAGE_LIST.begin(), LANGUAGE_LIST.end());
  return result;
}

int FindLanguage(std::string_view code)
{
  for (std::size_t index = 0; index < LANGUAGE_LIST.size(); ++index)
    if (code == LANGUAGE_LIST[index].code)
      return static_cast<int>(index);
  return -1;
}
}
