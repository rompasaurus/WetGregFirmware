/**
 * i18n.c — Egyptian Arabic UI strings.
 *
 * Purpose: the lookup table behind i18n_lookup(). Keys are the exact English
 * literals used by the screens (case and double spaces included). Values use
 * the same charset the Arabic font covers: letters, ٠-٩, ، ؟ and the ASCII
 * . - ! % : plus Latin letters for the joystick key hints (C = centre,
 * L = left, U/D = up/down, R = right) which stay as-is on purpose.
 *
 * Tone: everyday Cairo Arabic where a menu allows it (رجوع، اختار، يلا), plain
 * technical words where everyone uses the loanword (واي فاي، بلوتوث، IP).
 */
#include "i18n.h"

#include <string.h>

typedef struct { const char *en, *ar; } i18n_pair_t;

static const i18n_pair_t table[] = {
    /* ── Main menu ── */
    { "MENU",            "القائمة" },
    { "MOOD SELECT",     "اختار المزاج" },
    { "ANIMATIONS",      "الأنيميشن" },
    { "SOUND",           "الصوت" },
    { "MOTION",          "الحركة" },
    { "DEVICE INFO",     "معلومات الجهاز" },
    { "SOCIAL",          "الأصحاب" },
    { "SETTINGS",        "الإعدادات" },
    { "BACK",            "رجوع" },
    { "C:SELECT L:BACK", "C:اختيار  L:رجوع" },
    { "C:SELECT  L:BACK","C:اختيار  L:رجوع" },
    { "C:SEL L:BACK",    "C:اختيار  L:رجوع" },
    { "C:SEL  L:BACK",   "C:اختيار  L:رجوع" },
    { "L:BACK",          "L:رجوع" },
    { "LEFT:BACK",       "شمال:رجوع" },
    { "C:BACK",          "C:رجوع" },
    { "DOWN:MENU",       "تحت:القائمة" },
    { "STEPS",           "خطوات" },

    /* ── Mood select ── */
    { "SELECT MOOD",         "اختار المزاج" },
    { "ALL MOODS",           "كل المزاجات" },
    { "ALL MOODS (RANDOM)",  "كل المزاجات - عشوائي" },
    { "CURRENT",             "الحالي" },
    { "ALL",                 "الكل" },
    { "C:SET L:BACK",        "C:تثبيت  L:رجوع" },
    { "NORMAL",     "عادي" },      { "WEIRD",      "غريب" },
    { "UNHINGED",   "مجنون" },     { "ANGRY",      "متنرفز" },
    { "SAD",        "حزين" },      { "CHAOTIC",    "فوضوي" },
    { "HUNGRY",     "جعان" },      { "TIRED",      "تعبان" },
    { "SLAP HAPPY", "هايص" },      { "LAZY",       "كسلان" },
    { "FULL",       "شبعان" },     { "CHILL",      "رايق" },
    { "CREEPY",     "مرعب" },      { "EXCITED",    "متحمس" },
    { "NOSTALGIC",  "حنين" },      { "HOMESICK",   "واحشني البيت" },
    { "WISE",       "حكيم" },

    /* ── Settings ── */
    { "NETWORK",         "الشبكة" },
    { "BLUETOOTH",       "بلوتوث" },
    { "DISPLAY",         "الشاشة" },
    { "SET TIME",        "ضبط الوقت" },
    { "LANGUAGE",        "اللغة" },
    { "ENGLISH",         "إنجليزي" },
    { "ARABIC",          "عربي مصري" },
    { "RESET WETGREG",   "إعادة ضبط ويت جريج" },
    { "C:TOGGLE  L:BACK","C:تغيير  L:رجوع" },

    /* ── Display ── */
    { "AUTO-ROTATE",     "لف تلقائي" },
    { "ON",              "شغال" },
    { "OFF",             "مقفول" },
    { "ORIENT",          "الاتجاه" },
    { "WIDE",            "بالعرض" },
    { "WIDE FLIP",       "بالعرض مقلوب" },
    { "TALL",            "بالطول" },
    { "C:CYCLE  L:BACK", "C:بدل  L:رجوع" },

    /* ── Animations ── */
    { "INTRO",           "المقدمة" },
    { "C:PLAY  L:BACK",  "C:شغل  L:رجوع" },
    { "C:PLAY L:BACK",   "C:شغل  L:رجوع" },
    { "ANY KEY: EXIT",   "أي زرار: خروج" },

    /* ── Sound ── */
    { "TONE",            "النغمة" },
    { "VOLUME",          "الصوت" },
    { "LOW",             "واطي" },
    { "MED",             "متوسط" },
    { "HIGH",            "عالي" },
    { "C:PLAY  L/R:CHANGE TONE", "C:شغل  L/R:غير النغمة" },

    /* ── Device info ── */
    { "FW",              "الإصدار" },
    { "DISP",            "الشاشة" },
    { "BUILT",           "تاريخ البناء" },
    { "MOOD",            "المزاج" },
    { "QUOTES",          "الجمل" },
    { "WIFI",            "واي فاي" },
    { "USB",             "يو إس بي" },
    { "BATT",            "البطارية" },
    { "BATTERY",         "البطارية" },
    { "POWER",           "الكهربا" },
    { "LEFT",            "باقي" },
    { "RAW",             "القراءة" },
    { "UP:CAL FULL DN:RST",          "فوق:معايرة  تحت:مسح" },
    { "UP:CAL FULL=4.2V  DN:RESET CAL", "فوق:معايرة ممتلئ  تحت:مسح المعايرة" },

    /* ── Set time ── */
    { "SET DATE / TIME", "ضبط التاريخ والوقت" },
    { "YEAR",            "السنة" },
    { "MONTH",           "الشهر" },
    { "DAY",             "اليوم" },
    { "HOUR",            "الساعة" },
    { "MIN",             "الدقيقة" },
    { "U/D ADJUST  C OK  L BACK", "U/D غير  C تمام  L رجوع" },
    { "U/D FIELD  C EDIT  L BACK", "U/D اختار  C عدل  L رجوع" },

    /* ── Factory reset ── */
    { "RESET WETGREG?",      "إعادة ضبط ويت جريج؟" },
    { "ERASES ALL SETTINGS", "هيمسح كل الإعدادات" },
    { "AND PROGRESS:",       "وكل التقدم:" },
    { "- SAVED WIFI",        "- الواي فاي المحفوظ" },
    { "- NAME + FRIENDS",    "- الاسم والأصحاب" },
    { "- DISPLAY + SOCIAL",  "- الشاشة والأصحاب" },
    { "THEN REBOOTS AS NEW", "وبعدين يبدأ من جديد" },
    { "C:RESET  L:CANCEL",   "C:امسح  L:إلغاء" },

    /* ── Motion ── */
    { "ACCEL",           "التسارع" },
    { "TODAY",           "النهارده" },
    { "TILT",            "الميل" },
    { "RESET PEDOMETER", "صفر عداد الخطوات" },
    { "THRESHOLD",       "الحد" },
    { "I2C BUS SCAN",    "فحص I2C" },
    { "GYRO",            "الجيرو" },
    { "MAG",             "القوة" },

    /* ── Network ── */
    { "WIFI STATUS",     "حالة الواي فاي" },
    { "IP",              "IP" },          /* Latin on purpose: aligns the line RTL */
    { "NTP",             "NTP" },
    { "ID",              "الرقم" },
    { "MODE",            "الوضع" },
    { "SSID",            "الشبكة" },
    { "STATE",           "الحالة" },
    { "STATUS",          "الحالة" },
    { "SIGNAL",          "الإشارة" },
    { "TIME",            "الوقت" },
    { "ALWAYS ON",       "شغال دايما" },
    { "SYNC ONLY",       "للمزامنة بس" },
    { "CONNECTED",       "متوصل" },
    { "DISCONN",         "مش متوصل" },
    { "DISCONNECTED",    "مش متوصل" },
    { "DISABLED",        "مقفول" },
    { "SYNCED",          "متزامن" },
    { "NOT SYNCED",      "مش متزامن" },
    { "NEVER SYNCED",    "عمره ما اتزامن" },
    { "SYNCED JUST NOW", "اتزامن دلوقتي" },
    { "SCAN NETWORKS",   "دور على شبكات" },
    { "SAVED NETWORKS",  "الشبكات المحفوظة" },
    { "WIFI NETWORKS",   "شبكات الواي فاي" },
    { "SCANNING...",     "بدور..." },
    { "NO NETWORKS",     "مفيش شبكات" },
    { "NO NETWORKS FOUND", "مفيش شبكات" },
    { "FOUND",           "لقيت" },
    { "L:CANCEL",        "L:إلغاء" },
    { "LEFT:CANCEL",     "شمال:إلغاء" },
    { "C:JOIN L:BACK",   "C:اتصل  L:رجوع" },
    { "C:CONNECT",       "C:اتصل" },
    { "NONE SAVED",      "مفيش محفوظ" },
    { "C:JOIN R:FORGET L:BACK",     "C:اتصل  R:انسى  L:رجوع" },
    { "C:JOIN  R:FORGET  LEFT:BACK","C:اتصل  R:انسى  L:رجوع" },
    { "CONNECTING TO",   "بتصل بـ" },
    { "PLEASE WAIT...",  "استنى شوية..." },

    /* ── Bluetooth ── */
    { "ENTER THIS CODE ON",     "اكتب الكود ده على" },
    { "YOUR PHONE:",            "موبايلك:" },
    { "NAME",                   "الاسم" },
    { "STATUS: OFF (DISABLED)", "الحالة: مقفول" },
    { "BLUETOOTH IS OFF TO",    "البلوتوث مقفول عشان" },
    { "SAVE POWER.",            "نوفر البطارية." },
    { "C:ENABLE & PAIR  LEFT:BACK", "C:شغل واقرن  L:رجوع" },
    { "STARTING...",            "بيبدأ..." },
    { "DISCOVERABLE",           "ظاهر للموبايل" },
    { "PAIRED",                 "مقترن" },
    { "PEER",                   "الطرف التاني" },
    { "PAIRED! PHONE CAN NOW",  "تم الاقتران! الموبايل يقدر" },
    { "READ MOOD & STEPS.",     "يقرا المزاج والخطوات." },
    { "FIND \"WetGreg Hub\" ON", "دور على WetGreg Hub في" },
    { "YOUR PHONE & PAIR.",     "موبايلك واعمل اقتران." },
    { "C:DISABLE  LEFT:BACK",   "C:اقفل  L:رجوع" },

    /* ── Social ── */
    { "ME",                  "أنا" },
    { "SCAN",                "البحث" },
    { "SCAN NEARBY",         "مين حوالينا" },
    { "WETGREGS MET",        "ويت جريج قابلتهم" },
    { "SET NAME",            "غير الاسم" },
    { "U/D  C:SEL  L:BACK",  "U/D  C:اختيار  L:رجوع" },
    { "U/D  L:BACK",         "U/D  L:رجوع" },
    { "NONE YET - TURN ON SCAN", "لسه مفيش - شغل البحث" },
    { "SCANNING... NONE YET","بدور... لسه مفيش" },
    { "C:EMOTE  L:BACK",     "C:إيموت  L:رجوع" },
    { "ANOTHER WETGREG",     "ويت جريج تاني" },
    { "SAYS HELLO!",         "بيقولك أهلا!" },
    { "A WETGREG SAYS HELLO!","ويت جريج بيقولك أهلا!" },
    { "A WETGREG APPEARS",   "ويت جريج ظهر" },
    { "IN THE WILD!",        "قدامك!" },
    { "RESPOND?",            "ترد عليه؟" },
    { "SAY HI?",             "تقوله أهلا؟" },
    { "C:EMOTE  L:NO",       "C:إيموت  L:لا" },
    { "C:YES  L:NO",         "C:أيوه  L:لا" },
    { "PICK A NAME:",        "اختار اسم:" },
    { "U/D:REROLL C:SAVE L:NO", "U/D:غير  C:احفظ  L:لا" },
    { "SEND EMOTE",          "ابعت إيموت" },
    { "TO",                  "لـ" },
    { "U/D C:SEND L:BACK",   "U/D  C:ابعت  L:رجوع" },
    { "WAVE",                "تحية" },
    { "LOVE",                "حب" },
    { "LAUGH",               "ضحك" },
    { "PARTY",               "حفلة" },
    { "SLEEPY",              "نعسان" },
    { "WHOA",                "يا سلام" },
    { "WetGreg says hello!", "ويت جريج بيقولك أهلا!" },

    /* ── Splash / sleep / battery ── */
    { "WET GREG",            "ويت جريج" },
    { "WET",                 "ويت" },
    { "GREG",                "جريج" },
    { "GREG IS SLEEPING",    "جريج نايم" },
    { "GREG IS",             "جريج" },
    { "SLEEPING",            "نايم" },
    { "SHHHHH....",          "شششش...." },
    { "PRESS C TO WAKE HIM", "دوس C عشان تصحيه" },
    { "LOW BATT - CHARGE ME",          "البطارية فاضية - اشحني" },
    { "LOW BATTERY - PLEASE CHARGE ME","البطارية فاضية - اشحني لو سمحت" },
    { "CHARGE ME",           "اشحني" },
};

const char *i18n_lookup(const char *en) {
    if (!en || !*en) return NULL;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (strcmp(table[i].en, en) == 0) return table[i].ar;
    return NULL;
}

/* "Who is Greg" story, one beat per line (mirrors intro_lines[]). */
static const char *const intro_ar[] = {
    "في أعماق المحيط الأطلنطي...",
    "بيعوم أخطبوط لوحده...",
    "اسمه جريج...",
    "هو لوحده...",
    "وبعيد عن بيته.",
    "وهو مبلول...",
};

const char *i18n_intro_line(int i) {
    int n = (int)(sizeof(intro_ar) / sizeof(intro_ar[0]));
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return intro_ar[i];
}
