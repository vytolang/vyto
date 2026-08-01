package dev.vyto.android;

import android.icu.text.BreakIterator;
import android.icu.text.Collator;
import android.icu.text.DateFormat;
import android.icu.text.Normalizer2;
import android.icu.text.NumberFormat;
import android.icu.text.PluralRules;
import android.icu.util.Currency;
import android.icu.util.TimeZone;
import android.icu.util.ULocale;

import java.nio.charset.StandardCharsets;
import java.util.Date;

/**
 * ICU for {@code vyto/mobile/android/intl}, through {@code android.icu.*}.
 *
 * <p>Called from {@code native/src/aintl_shim.c}. Every method here is static
 * and every handle is a plain {@code Object} the C side holds as a JNI global
 * ref — so the shim needs no per-type class lookups, only this one class.
 *
 * <p>STATUS: written, never compiled. Design of record:
 * {@code local/docs/ANDROID.md}.
 *
 * <h2>Why android.icu and not the C libicuuc</h2>
 *
 * The device's ICU exports version-suffixed private symbols the NDK does not
 * publish. A binary that links them loads today and fails on the next platform
 * release. This is the supported door to the same tables.
 *
 * <h2>Failures are null, not exceptions</h2>
 *
 * Nothing throws across JNI. A method that cannot do its job logs and returns
 * null (or a neutral value), and the C side turns that into the -1 the Vyto
 * side already knows how to handle.
 */
public final class Intl {

    private Intl() {}

    // Must match the const blocks in vyto/mobile/android/intl.vt.
    private static final int NFC = 0, NFD = 1, NFKC = 2, NFKD = 3;
    private static final int NUM_DECIMAL = 0, NUM_CURRENCY = 1,
                             NUM_PERCENT = 2, NUM_SCIENTIFIC = 3;
    private static final int DT_FULL = 0, DT_LONG = 1, DT_MEDIUM = 2,
                             DT_SHORT = 3, DT_NONE = 4;
    private static final int BRK_GRAPHEME = 0, BRK_WORD = 1, BRK_LINE = 2;
    private static final int CASE_UPPER = 0, CASE_LOWER = 1, CASE_FOLD = 2;
    private static final int PLURAL_CARDINAL = 0, PLURAL_ORDINAL = 1;

    private static void warn(String what, Throwable t) {
        android.util.Log.w("Vyto", "intl " + what + " failed: " + t);
    }

    /**
     * The device locale as an ICU id. Not the POSIX environment: on Android the
     * user's language is a system setting (per-app overridable since 13), and
     * $LANG says nothing about it.
     */
    public static String defaultLocale() {
        try {
            return ULocale.getDefault().getName();
        } catch (Throwable t) {
            warn("defaultLocale", t);
            return "en_US";
        }
    }

    // -------------------------------------------------------------- numbers

    public static Object numOpen(String locale, int style) {
        try {
            ULocale l = new ULocale(locale);
            switch (style) {
                case NUM_CURRENCY:   return NumberFormat.getCurrencyInstance(l);
                case NUM_PERCENT:    return NumberFormat.getPercentInstance(l);
                case NUM_SCIENTIFIC: return NumberFormat.getScientificInstance(l);
                default:             return NumberFormat.getNumberInstance(l);
            }
        } catch (Throwable t) {
            warn("numOpen " + locale, t);
            return null;
        }
    }

    public static String numFmtDouble(Object h, double v) {
        try {
            return ((NumberFormat) h).format(v);
        } catch (Throwable t) {
            warn("numFmtDouble", t);
            return null;
        }
    }

    public static String numFmtLong(Object h, long v) {
        try {
            return ((NumberFormat) h).format(v);
        } catch (Throwable t) {
            warn("numFmtLong", t);
            return null;
        }
    }

    /**
     * Money. The currency is set per call rather than at open, because the
     * formatter carries the locale's currency by default and the caller means a
     * specific one — a EUR amount shown to a US user is "€1,234.50", which is
     * this instance with the currency swapped, not the German formatter.
     */
    public static String numFmtCurrency(Object h, double v, String iso3) {
        try {
            NumberFormat f = (NumberFormat) h;
            if (iso3 != null && iso3.length() == 3) {
                f.setCurrency(Currency.getInstance(iso3));
            }
            return f.format(v);
        } catch (Throwable t) {
            warn("numFmtCurrency " + iso3, t);
            return null;
        }
    }

    /** -1 leaves a side at the locale default, matching the Vyto signature. */
    public static void numSetFraction(Object h, int minFrac, int maxFrac) {
        try {
            NumberFormat f = (NumberFormat) h;
            if (minFrac >= 0) f.setMinimumFractionDigits(minFrac);
            if (maxFrac >= 0) f.setMaximumFractionDigits(maxFrac);
        } catch (Throwable t) {
            warn("numSetFraction", t);
        }
    }

    // ---------------------------------------------------------------- dates

    private static int icuStyle(int s) {
        switch (s) {
            case DT_FULL:   return DateFormat.FULL;
            case DT_LONG:   return DateFormat.LONG;
            case DT_MEDIUM: return DateFormat.MEDIUM;
            case DT_SHORT:  return DateFormat.SHORT;
            default:        return DateFormat.NONE;
        }
    }

    public static Object datOpen(String locale, int dateStyle, int timeStyle, String tz) {
        try {
            ULocale l = new ULocale(locale);
            int ds = icuStyle(dateStyle);
            int ts = icuStyle(timeStyle);
            DateFormat f;
            if (ds == DateFormat.NONE && ts == DateFormat.NONE) {
                f = DateFormat.getDateInstance(DateFormat.MEDIUM, l);
            } else if (ts == DateFormat.NONE) {
                f = DateFormat.getDateInstance(ds, l);
            } else if (ds == DateFormat.NONE) {
                f = DateFormat.getTimeInstance(ts, l);
            } else {
                f = DateFormat.getDateTimeInstance(ds, ts, l);
            }
            // "" means the device zone, which is already the default.
            if (tz != null && !tz.isEmpty()) {
                f.setTimeZone(TimeZone.getTimeZone(tz));
            }
            return f;
        } catch (Throwable t) {
            warn("datOpen " + locale, t);
            return null;
        }
    }

    public static String datFmt(Object h, long unixMs) {
        try {
            return ((DateFormat) h).format(new Date(unixMs));
        } catch (Throwable t) {
            warn("datFmt", t);
            return null;
        }
    }

    // ----------------------------------------------------- normalize / case

    public static String normalize(String s, int mode) {
        try {
            Normalizer2 n;
            switch (mode) {
                case NFD:  n = Normalizer2.getNFDInstance();  break;
                case NFKC: n = Normalizer2.getNFKCInstance(); break;
                case NFKD: n = Normalizer2.getNFKDInstance(); break;
                default:   n = Normalizer2.getNFCInstance();  break;
            }
            return n.normalize(s);
        } catch (Throwable t) {
            warn("normalize", t);
            return null;
        }
    }

    /**
     * Case mapping. Upper/lower are locale-sensitive on purpose — Turkish
     * dotted and dotless i are wrong under the root locale — while fold is
     * for comparison and must not vary by locale.
     */
    public static String caseMap(String s, String locale, int op) {
        try {
            if (op == CASE_FOLD) {
                return android.icu.lang.UCharacter.foldCase(s, true);
            }
            ULocale l = (locale == null || locale.isEmpty())
                    ? ULocale.ROOT : new ULocale(locale);
            return op == CASE_UPPER
                    ? android.icu.lang.UCharacter.toUpperCase(l, s)
                    : android.icu.lang.UCharacter.toLowerCase(l, s);
        } catch (Throwable t) {
            warn("caseMap", t);
            return null;
        }
    }

    // ---------------------------------------------------------- break walk

    /**
     * A break iterator plus the string it walks.
     *
     * <p>ICU indexes UTF-16 and the Vyto side slices UTF-8, so the two disagree
     * about every offset past the first non-ASCII character. Converting here is
     * the only place both encodings of the string are in hand — doing it on the
     * C side would mean shipping the string across JNI a second time.
     */
    private static final class Brk {
        final BreakIterator it;
        final String s;
        Brk(BreakIterator it, String s) {
            this.it = it;
            this.s = s;
            it.setText(s);
        }
        /** UTF-8 byte offset of the next boundary, or -1 at the end. */
        int next() {
            int i = it.next();
            if (i == BreakIterator.DONE) return -1;
            // Byte length of the prefix, which is the UTF-8 offset.
            return s.substring(0, i).getBytes(StandardCharsets.UTF_8).length;
        }
    }

    public static Object brkOpen(int kind, String locale, String s) {
        try {
            ULocale l = (locale == null || locale.isEmpty())
                    ? ULocale.getDefault() : new ULocale(locale);
            BreakIterator it;
            switch (kind) {
                case BRK_WORD: it = BreakIterator.getWordInstance(l); break;
                case BRK_LINE: it = BreakIterator.getLineInstance(l); break;
                default:       it = BreakIterator.getCharacterInstance(l); break;
            }
            return new Brk(it, s == null ? "" : s);
        } catch (Throwable t) {
            warn("brkOpen", t);
            return null;
        }
    }

    public static int brkNext(Object h) {
        try {
            return ((Brk) h).next();
        } catch (Throwable t) {
            warn("brkNext", t);
            return -1;
        }
    }

    // ------------------------------------------------------------ collation

    public static Object colOpen(String locale) {
        try {
            return Collator.getInstance(new ULocale(locale));
        } catch (Throwable t) {
            warn("colOpen " + locale, t);
            return null;
        }
    }

    public static int colCompare(Object h, String a, String b) {
        try {
            int r = ((Collator) h).compare(a, b);
            // ICU already returns -1/0/1, but the contract is explicit about it
            // and a RuleBasedCollator subclass need not be.
            return r < 0 ? -1 : (r > 0 ? 1 : 0);
        } catch (Throwable t) {
            warn("colCompare", t);
            return 0;
        }
    }

    public static byte[] colSortKey(Object h, String s) {
        try {
            return ((Collator) h).getCollationKey(s).toByteArray();
        } catch (Throwable t) {
            warn("colSortKey", t);
            return null;
        }
    }

    // -------------------------------------------------------------- plurals

    public static Object pluralOpen(String locale, int kind) {
        try {
            return PluralRules.forLocale(new ULocale(locale),
                    kind == PLURAL_ORDINAL ? PluralRules.PluralType.ORDINAL
                                           : PluralRules.PluralType.CARDINAL);
        } catch (Throwable t) {
            warn("pluralOpen " + locale, t);
            return null;
        }
    }

    public static String pluralSelect(Object h, double v) {
        try {
            return ((PluralRules) h).select(v);
        } catch (Throwable t) {
            warn("pluralSelect", t);
            return null;
        }
    }
}
