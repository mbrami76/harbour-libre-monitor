// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Michele Brami
// Checks of the translation files (REQ-UI-13). What they prove: every
// language file has every text of the English source, nothing is left untranslated, the
// placeholders (%1, %2, %n) are the same in the source and in each translation, and plural
// texts have the forms the language needs. A new language is "added" by copying a file; this is
// the test that tells the translator what is still missing. The dummy-language test shows the
// procedure works for a language nobody wrote.
#include <QtTest>

#include <QDir>
#include <QRegularExpression>
#include <QXmlStreamReader>

namespace {

struct Message
{
    QString id, source;
    bool numerus = false;
    bool unfinished = false;
    QStringList forms;          // the translation, or its plural forms
};

QMap<QString, Message> readTs(const QString &path, QString *language = 0)
{
    QMap<QString, Message> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    QXmlStreamReader x(&f);
    Message cur;
    bool inTranslation = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            if (x.name() == "TS" && language)
                *language = x.attributes().value("language").toString();
            if (x.name() == "message") {
                cur = Message();
                cur.id = x.attributes().value("id").toString();
                cur.numerus = x.attributes().value("numerus") == "yes";
            } else if (x.name() == "source") {
                cur.source = x.readElementText();
            } else if (x.name() == "translation") {
                inTranslation = true;
                cur.unfinished = x.attributes().value("type") == "unfinished";
                if (!cur.numerus)
                    cur.forms << x.readElementText();
            } else if (x.name() == "numerusform" && inTranslation) {
                cur.forms << x.readElementText();
            }
        } else if (x.isEndElement()) {
            if (x.name() == "translation") inTranslation = false;
            if (x.name() == "message") out.insert(cur.id, cur);
        }
    }
    return out;
}

QStringList placeholders(const QString &t)
{
    QStringList p;
    QRegularExpressionMatchIterator it = QRegularExpression("%(n|[1-9])").globalMatch(t);
    while (it.hasNext()) p << it.next().captured(0);
    p.sort();
    return p;
}

QStringList languageFiles()
{
    return QDir(LM_TRANSLATIONS_DIR).entryList(QStringList() << "harbour-libre-monitor*.ts", QDir::Files, QDir::Name);
}

}

class TstTranslations : public QObject
{
    Q_OBJECT

private slots:
    void englishSourceExistsAndHasTexts()
    {
        const QMap<QString, Message> en = readTs(QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor.ts");
        QVERIFY2(en.size() > 50, "the English source file is missing or empty");
        for (QMap<QString, Message>::const_iterator i = en.begin(); i != en.end(); ++i) {
            QVERIFY2(!i->source.isEmpty(), qPrintable(i->id));
            QVERIFY2(!i->id.isEmpty(), "a text without an identifier");
        }
    }
    void everyLanguageIsCompleteAndConsistent()
    {
        const QMap<QString, Message> en = readTs(QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor.ts");
        const QStringList files = languageFiles();
        QVERIFY(files.size() >= 2);                     // English and Italian
        for (int f = 0; f < files.size(); ++f) {
            const QMap<QString, Message> tr = readTs(QString(LM_TRANSLATIONS_DIR) + "/" + files.at(f));
            // Every identifier of the source exists, finished and non-empty.
            for (QMap<QString, Message>::const_iterator i = en.begin(); i != en.end(); ++i) {
                const QString where = files.at(f) + ": " + i->id;
                QVERIFY2(tr.contains(i->id), qPrintable(where + " is missing"));
                const Message &m = tr[i->id];
                QVERIFY2(!m.unfinished, qPrintable(where + " is unfinished"));
                QVERIFY2(!m.forms.isEmpty(), qPrintable(where + " has no translation"));
                for (int k = 0; k < m.forms.size(); ++k) {
                    QVERIFY2(!m.forms.at(k).isEmpty(), qPrintable(where + " is empty"));
                    // The same placeholders as the source (a plural form may drop %n for "one").
                    if (!m.numerus)
                        QCOMPARE(placeholders(m.forms.at(k)), placeholders(i->source));
                    else
                        for (int p = 0; p < placeholders(m.forms.at(k)).size(); ++p)
                            QVERIFY2(placeholders(i->source).contains(placeholders(m.forms.at(k)).at(p)),
                                     qPrintable(where + " has a placeholder the source has not"));
                }
                if (m.numerus)
                    QVERIFY2(m.forms.size() >= 2, qPrintable(where + " needs two plural forms"));
            }
            // And nothing the source does not have.
            for (QMap<QString, Message>::const_iterator i = tr.begin(); i != tr.end(); ++i)
                QVERIFY2(en.contains(i->id), qPrintable(files.at(f) + ": extra " + i->id));
        }
    }
    // (REQ-UI-16): the pages use the words of the notifications. The levels are very low,
    // low, high, very high and no data, the monitored person is a patient. No text a user can see, in
    // any language file, may carry the band abbreviations (LL, L, N, H, HH as words of their own), the
    // word "urgent" or the word "Subject" ("soggetto" in Italian): they stay in the code, the text
    // identifiers and the specification only. The rule is checked on the English source and on every
    // translation, so a new language is held to it as well.
    void noForbiddenWordIsShownInAnyLanguage()
    {
        // A capital L, N or H standing alone (not inside "mg/dL", not the Italian "L’" before a vowel).
        const QRegularExpression abbrev(QString::fromUtf8("(?<![\\w/’'])(LL|HH|L|N|H)(?![\\w’'/])"));
        // the user texts do not say "rebuild" (Italian "ricostruzione"); the space compaction needs
        // while it runs is "temporary space needed during compaction".
        const QRegularExpression words("urgen|subject|soggett|rebuil|ricostru", QRegularExpression::CaseInsensitiveOption);
        // The rule itself catches what it must and lets the normal words pass.
        QVERIFY(abbrev.match("Below the L limit").hasMatch());
        QVERIFY(abbrev.match("HH").hasMatch());
        QVERIFY(words.match("Urgent low").hasMatch() && words.match("Subject 1").hasMatch());
        QVERIFY(words.match("The rebuild after it").hasMatch() && words.match("la ricostruzione").hasMatch());
        QVERIFY(!abbrev.match(QString::fromUtf8("Below 70 mg/dL · L’accesso · P1 · %n h ago")).hasMatch());
        QVERIFY(!words.match("Very low, low, high, very high, no data, patient").hasMatch());

        const QStringList files = languageFiles();
        for (int f = 0; f < files.size(); ++f) {
            const QMap<QString, Message> tr = readTs(QString(LM_TRANSLATIONS_DIR) + "/" + files.at(f));
            for (QMap<QString, Message>::const_iterator i = tr.begin(); i != tr.end(); ++i) {
                QStringList shown = i->forms;
                shown << i->source;
                for (int k = 0; k < shown.size(); ++k) {
                    const QString where = files.at(f) + ": " + i->id + ": \"" + shown.at(k) + "\"";
                    QVERIFY2(!abbrev.match(shown.at(k)).hasMatch(), qPrintable(where + " shows a band abbreviation"));
                    QVERIFY2(!words.match(shown.at(k)).hasMatch(), qPrintable(where + " shows a forbidden word"));
                }
            }
        }
    }
    // every text identifier the pages use exists in the English source file (and so, by
    // the test above, in every language, checked for forbidden words). The package build's lupdate adds new
    // identifiers by itself, but the files in the repository are what a host test and a translator read, so
    // a text added to a page without its entry would otherwise go unnoticed until a package is built.
    // Fifteen English texts and their Italian are fixed. They are used word for word under their
    // keys; a change of their wording is a deliberate decision, not a side effect.
    void theApprovedTextsAreUsedVerbatim()
    {
        struct Approved { const char *id, *en, *it; };
        static const Approved approved[] = {
            { "ui-service-older", "The monitoring service is outdated. Stop and Start monitoring again.", "Il servizio di monitoraggio è obsoleto. Ferma e avvia di nuovo il monitoraggio." },  // #1
            { "ui-strip-stop-button", "Stop monitoring", "Ferma il monitoraggio" },  // #2
            { "ui-service-newer", "The app is older than the service. Reopen it.", "L'app è più vecchia del servizio. Riaprila." },  // #3
            { "ui-storage-compact-confirm", "Of the current readings older than %1, one every 5 minutes is kept. Everything else (history graph, logbook, marks, events, sensors) is untouched.", "Delle letture correnti più vecchie di %1 ne resta una ogni 5 minuti. Il resto (grafico storico, registro, segni, eventi, sensori) non viene toccato." },  // #4
            { "ui-storage-compact-permanent", "Removed data cannot be restored by the app. The vendor's service may still hold the readings.", "I dati eliminati non si possono ripristinare dall'app. Il servizio del produttore potrebbe conservare le letture." },  // #5
            { "ui-storage-window-hint", "Readings of the last 14 days are never thinned. Removed readings cannot be restored.", "Le letture degli ultimi 14 giorni non vengono mai sfoltite. Quelle eliminate non si ripristinano." },  // #6
            { "ui-storage-space-short", "Not enough free space: about %1 more is needed.", "Spazio libero insufficiente: servono circa %1 in più." },  // #7
            { "ui-storage-space-unknown", "Free space cannot be read. Nothing was removed.", "Impossibile leggere lo spazio libero. Non è stato eliminato nulla." },  // #8
            { "ui-storage-nothing", "Nothing to compact for this window.", "Niente da compattare per questo periodo." },  // #9
            { "ui-storage-low", "Low storage. Compaction would free about %1.", "Poca memoria. La compattazione libererebbe circa %1." },  // #10
            { "ui-storage-low-noestimate", "Low storage. Compaction can free space.", "Poca memoria. La compattazione può liberare spazio." },  // #11
            { "ui-maint-fail-insufficient-space-missing", "Not enough free space: about %1 more is needed. Nothing was changed.", "Spazio libero insufficiente: servono circa %1 in più. Nulla è stato modificato." },  // #12
            { "ui-maint-fail-free-space-unknown", "Free space could not be checked. Nothing was removed. Try again later.", "Impossibile controllare lo spazio libero. Non è stato eliminato nulla. Riprova più tardi." },  // #13
            { "ui-maint-fail-io", "The data could not be read or written. Nothing was changed.", "Impossibile leggere o scrivere i dati. Nulla è stato modificato." },  // #14
            { "ui-storage-estimate", "Estimate: frees about %1. Temporary space needed: %2.", "Stima: libera circa %1. Spazio temporaneo necessario: %2." },  // #15
        };
        const QMap<QString, Message> en = readTs(QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor.ts");
        const QMap<QString, Message> it = readTs(QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor-it.ts");
        for (unsigned i = 0; i < sizeof(approved) / sizeof(approved[0]); ++i) {
            const QString id = QString::fromLatin1(approved[i].id);
            QVERIFY2(en.contains(id) && it.contains(id), qPrintable(id + " is missing"));
            QCOMPARE(en[id].source, QString::fromUtf8(approved[i].en));
            QCOMPARE(it[id].forms.value(0), QString::fromUtf8(approved[i].it));
        }
    }
    void everyIdentifierThePagesUseIsInTheFiles()
    {
        const QMap<QString, Message> en = readTs(QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor.ts");
        QDirIterator it(QString(LM_TRANSLATIONS_DIR) + "/../qml", QStringList() << "*.qml" << "*.js",
                        QDir::Files, QDirIterator::Subdirectories);
        const QRegularExpression id("qsTrId\\(\\s*\"([^\"]+)\"");
        int files = 0, ids = 0;
        while (it.hasNext()) {
            QFile f(it.next());
            QVERIFY(f.open(QIODevice::ReadOnly));
            ++files;
            // Line by line, comment lines left out (a comment may quote the pattern itself).
            const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            for (int l = 0; l < lines.size(); ++l) {
                if (lines.at(l).trimmed().startsWith(QLatin1String("//")))
                    continue;
                QRegularExpressionMatchIterator m = id.globalMatch(lines.at(l));
                while (m.hasNext()) {
                    const QString key = m.next().captured(1);
                    ++ids;
                    QVERIFY2(en.contains(key), qPrintable(QFileInfo(f.fileName()).fileName() + ": " + key + " is not in the translation files"));
                }
            }
        }
        QVERIFY(files > 20 && ids > 150);
    }
    void aNewLanguageIsOneCopiedFile()
    {
        // The documented procedure: copy the English file, mark the language, translate. This
        // builds a dummy language the way a translator would and checks that the same rules
        // can be applied to it: it starts with every text "unfinished" (nothing is missing,
        // nothing is done), which is exactly what the report for a translator lists.
        QTemporaryDir dir;
        const QString from = QString(LM_TRANSLATIONS_DIR) + "/harbour-libre-monitor.ts";
        const QString to = dir.path() + "/harbour-libre-monitor-xx.ts";
        QVERIFY(QFile::copy(from, to));
        QString language;
        const QMap<QString, Message> dummy = readTs(to, &language);
        const QMap<QString, Message> en = readTs(from);
        QCOMPARE(dummy.keys(), en.keys());              // every identifier carried over
    }
};

QTEST_APPLESS_MAIN(TstTranslations)
#include "tst_translations.moc"
