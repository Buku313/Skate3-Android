package chat.buku.skate3;

import android.content.Context;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** Small runtime string layer while the programmatic launcher moves to Android resources. */
final class LauncherStrings {
    static final String ENGLISH = "en";
    static final String PORTUGUESE_BRAZIL = "pt-BR";

    private static final String PREFERENCES = "launcher-language";
    private static final String LANGUAGE = "language";
    private static final Map<String, String> PT = new HashMap<>();

    static {
        put("PHONE-ONLY INSTALLER", "INSTALADOR SEM COMPUTADOR");
        put("No computer required. Select an Xbox 360 ISO that you dumped from your own Skate 3 copy. The game stays on this device.",
            "Nenhum computador é necessário. Selecione uma ISO de Xbox 360 extraída da sua própria cópia de Skate 3. O jogo permanece neste dispositivo.");
        put("Checking installation...", "Verificando a instalação...");
        put("CHECK FOR APP UPDATES", "PROCURAR ATUALIZAÇÕES");
        put("CHECKING FOR UPDATES...", "PROCURANDO ATUALIZAÇÕES...");
        put("APP UP TO DATE", "APLICATIVO ATUALIZADO");
        put("BUG REPORT / DEVICE DIAGNOSTICS", "RELATAR BUG / DIAGNÓSTICO DO DISPOSITIVO");
        put("Requires Android 13+, ARM64, Vulkan, and about 8 GB free after the ISO is already on your device or USB drive. Touch controls are included.",
            "Requer Android 13+, ARM64, Vulkan e cerca de 8 GB livres além da ISO no dispositivo ou unidade USB. Controles de toque estão incluídos.");
        put("DEVICE NOT SUPPORTED", "DISPOSITIVO NÃO COMPATÍVEL");
        put("This build requires a 64-bit ARM Android device.", "Esta versão requer um dispositivo Android ARM de 64 bits.");
        put("This build requires Android 13 or newer.", "Esta versão requer Android 13 ou mais recente.");
        put("This device does not report the required Vulkan support.", "Este dispositivo não informa o suporte Vulkan necessário.");
        put("CLOSE", "FECHAR");
        put("READY TO SKATE", "PRONTO PARA ANDAR DE SKATE");
        put("PLAY SKATE 3", "JOGAR SKATE 3");
        put("REPAIR OR REINSTALL", "REPARAR OU REINSTALAR");
        put("VIEW SETUP LOG", "VER REGISTRO DA INSTALAÇÃO");
        put("VIEW LAST SETUP LOG", "VER ÚLTIMO REGISTRO");
        put("GAME EXTRACTED", "JOGO EXTRAÍDO");
        put("Finish by downloading the verified 1.7 MB Title Update 3, or select the package yourself.",
            "Finalize baixando a Title Update 3 verificada de 1,7 MB ou selecione o pacote manualmente.");
        put("FINISH SETUP AUTOMATICALLY", "FINALIZAR INSTALAÇÃO AUTOMATICAMENTE");
        put("SELECT TITLE UPDATE FILE", "SELECIONAR ARQUIVO DA TITLE UPDATE");
        put("START OVER", "RECOMEÇAR");
        put("ONE FILE NEEDED", "FALTA UM ARQUIVO");
        put("SELECT MY SKATE 3 ISO", "SELECIONAR MINHA ISO DE SKATE 3");
        put("INSPECTING ISO", "ANALISANDO A ISO");
        put("EXTRACTING GAME", "EXTRAINDO O JOGO");
        put("INSTALLING TITLE UPDATE 3", "INSTALANDO TITLE UPDATE 3");
        put("VERIFYING TITLE UPDATE 3", "VERIFICANDO TITLE UPDATE 3");
        put("Downloading and verifying 1.7 MB...", "Baixando e verificando 1,7 MB...");
        put("The Title Update server is temporarily unavailable. The app tried 3 times. Try again later, or use Select Title Update File.",
            "O servidor da Title Update está temporariamente indisponível. O aplicativo tentou 3 vezes. Tente novamente mais tarde ou use Selecionar Arquivo da Title Update.");
        put("Title Update download was interrupted.", "O download da Title Update foi interrompido.");
        put("INSTALLATION COMPLETE", "INSTALAÇÃO CONCLUÍDA");
        put("Your files were verified and stayed on this device.", "Seus arquivos foram verificados e permaneceram neste dispositivo.");
        put("Game installation needs repair.", "A instalação do jogo precisa ser reparada.");
        put("You already have the newest build.", "Você já possui a versão mais recente.");
        put("DOWNLOADING APP UPDATE", "BAIXANDO ATUALIZAÇÃO DO APLICATIVO");
        put("UPDATE VERIFIED", "ATUALIZAÇÃO VERIFICADA");
        put("Android will now open the installer. Your game files stay in place.",
            "O Android abrirá o instalador. Os arquivos do jogo permanecerão no lugar.");
        put("Allow installs from Skate 3, then return here.", "Permita instalações pelo Skate 3 e depois volte aqui.");
        put("Later", "Depois");
        put("Download update", "Baixar atualização");
        put("The app will verify the download, then Android will ask you to tap Install. Your game files and settings stay in place.",
            "O aplicativo verificará o download e o Android pedirá que você toque em Instalar. Os arquivos e configurações do jogo permanecerão no lugar.");
        put("CHARACTERS  •  SEIYU INCLUDED", "PERSONAGENS  •  SEIYU INCLUÍDO");
        put("CHARACTERS  •  RESTORE SEIYU", "PERSONAGENS  •  RESTAURAR SEIYU");
        put("PREPARING SEIYU", "PREPARANDO SEIYU");
        put("Installing the included playable penguin...", "Instalando o pinguim jogável incluído...");
        put("ADVANCED OPTIONS", "OPÇÕES AVANÇADAS");
        put("ADVANCED OPTIONS  •  CUSTOM GPU DRIVER ACTIVE", "OPÇÕES AVANÇADAS  •  DRIVER DE GPU PERSONALIZADO ATIVO");
        put("CLEANING UP", "LIMPANDO");
        put("Removing this app's installed copy...", "Removendo a cópia instalada por este aplicativo...");
        put("Setup needs attention", "A instalação precisa de atenção");
        put("Your original ISO was not changed.", "Sua ISO original não foi alterada.");
        put("Repair or reinstall?", "Reparar ou reinstalar?");
        put("This removes the extracted game files from this app, then lets you select your ISO again. Your original ISO is not changed.",
            "Isso remove os arquivos extraídos pelo aplicativo e permite selecionar a ISO novamente. Sua ISO original não será alterada.");
        put("Discard partial setup?", "Descartar instalação parcial?");
        put("Only the incomplete copy created by this installer will be removed. Your original ISO is not changed.",
            "Somente a cópia incompleta criada pelo instalador será removida. Sua ISO original não será alterada.");
        put("Cancel", "Cancelar");
        put("Continue", "Continuar");
        put("Start over", "Recomeçar");
        put("Setup log", "Registro da instalação");
        put("No setup log has been written yet.", "Nenhum registro de instalação foi criado ainda.");
        put("Launcher language", "Idioma do launcher");
        put("Language changed. The launcher will restart.", "Idioma alterado. O launcher será reiniciado.");
        put("selected file", "arquivo selecionado");
        put("Open Buku313 on GitHub", "Abrir Buku313 no GitHub");
        put("Report a developer-build bug", "Relatar um bug da versão de desenvolvimento");
        put("GitHub will open with this device's safe technical details already filled in. The same details will be copied so you can paste them if the browser removes a field.\n\nNo ISO, game file, save, account name, or private path is included.",
            "O GitHub abrirá com os detalhes técnicos seguros deste dispositivo já preenchidos. Os mesmos detalhes serão copiados para você colar caso o navegador remova algum campo.\n\nNenhuma ISO, arquivo do jogo, save, nome de conta ou caminho privado será incluído.");
        put("Copy only", "Somente copiar");
        put("Open GitHub", "Abrir GitHub");
        put("Device diagnostics copied.", "Diagnóstico do dispositivo copiado.");
        put("No browser is available. The diagnostics are copied.", "Nenhum navegador está disponível. O diagnóstico foi copiado.");
    }

    private LauncherStrings() {}

    static boolean isPortuguese(Context context) {
        return PORTUGUESE_BRAZIL.equals(language(context));
    }

    static String language(Context context) {
        String saved = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
            .getString(LANGUAGE, null);
        if (saved != null) return saved;
        return "pt".equalsIgnoreCase(Locale.getDefault().getLanguage())
            ? PORTUGUESE_BRAZIL : ENGLISH;
    }

    static void setLanguage(Context context, String language) {
        String selected = PORTUGUESE_BRAZIL.equals(language)
            ? PORTUGUESE_BRAZIL : ENGLISH;
        context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
            .edit().putString(LANGUAGE, selected).apply();
    }

    static String languageButton(Context context) {
        return isPortuguese(context) ? "🇺🇸 English" : "🇧🇷 Português";
    }

    static String text(Context context, String english) {
        if (!isPortuguese(context) || english == null || english.isEmpty()) return english;
        String exact = PT.get(english);
        if (exact != null) return exact;

        if (english.startsWith("UPDATE TO ")) {
            return "ATUALIZAR PARA " + english.substring("UPDATE TO ".length());
        }
        if (english.startsWith("Game files: ")) {
            return "Arquivos do jogo: " + english.substring("Game files: ".length());
        }
        if (english.contains("\nGame files: ")) {
            return english.replace("\nGame files: ", "\nArquivos do jogo: ");
        }
        if (english.startsWith("Select your Skate 3 Xbox 360 ISO.")) {
            return english.replace(
                "Select your Skate 3 Xbox 360 ISO. It can be in Downloads, on an SD card, or on a connected USB drive.",
                "Selecione sua ISO de Skate 3 para Xbox 360. Ela pode estar em Downloads, em um cartão SD ou em uma unidade USB conectada.")
                .replace("Free space: ", "Espaço livre: ");
        }
        if (english.startsWith("Checking ")) return "Verificando " + english.substring(9);
        if (english.startsWith("Reading ")) return "Lendo " + english.substring(8);
        if (english.startsWith("Starting ")) return "Iniciando " + english.substring(9);
        if (english.startsWith("Downloading Skate 3 ")) {
            return "Baixando Skate 3 " + english.substring("Downloading Skate 3 ".length());
        }
        if (english.startsWith("Retrying Title Update download (")) {
            return english.replace("Retrying Title Update download (",
                "Tentando baixar a Title Update novamente (")
                .replace(" of ", " de ");
        }
        if (english.startsWith("Title Update setup stopped: ")) {
            return "A instalação da Title Update foi interrompida: " +
                text(context, english.substring("Title Update setup stopped: ".length()));
        }
        if (english.startsWith("Setup stopped: ")) {
            return "A instalação foi interrompida: " +
                text(context, english.substring("Setup stopped: ".length()));
        }
        if (english.contains(" of ")) return english.replace(" of ", " de ");
        return english;
    }

    private static void put(String english, String portuguese) {
        PT.put(english, portuguese);
    }
}
