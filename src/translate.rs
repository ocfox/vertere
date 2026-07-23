//! The translation prompt and the model's streamed reply.

/// Marks the end of the translation and the start of the source text.
///
/// A single character with no plausible reason to appear in ordinary prose,
/// which keeps the split unambiguous without escaping. Being a single character
/// also means it can never straddle two streamed deltas.
pub const SEPARATOR: char = '⁂';

/// Builds the system prompt for translating into `target_lang`.
///
/// `fallback_lang`, when set, is what to use if the source is already in
/// `target_lang`. One native language and one working language covers most of
/// what a person reads, and letting the model pick the direction beats making
/// the user pick it on every keystroke.
pub fn system_prompt(target_lang: &str, fallback_lang: &str) -> String {
    let fallback = fallback_lang.trim();
    // Part of the opening directive rather than a rule buried in the list
    // further down: the exception competes with "translate into {target_lang}"
    // for the model's attention, and loses when it is not stated in the same
    // breath as the rule it is an exception to.
    let exception = if fallback.is_empty() {
        "repeat it unchanged".to_string()
    } else {
        format!("translate it into {fallback} instead")
    };
    let directive = format!(
        "You translate text into {target_lang}, unless the source is already \
written in {target_lang} — judged by its grammar, not merely by sharing \
characters with it — in which case you {exception}."
    );

    // The translation comes first so the user sees it while the transcription is
    // still arriving; the transcription earns its keep by making history
    // searchable and re-translatable without a separate OCR pass.
    format!(
        "\
{directive} The text reaches you either as an image of a screen region or as \
plain text.

Reply with exactly two sections, separated by a line containing only {SEPARATOR}:

<the translation>
{SEPARATOR}
<the source text, transcribed exactly as it appears>

Rules:
- Output nothing else. No preamble, no commentary, no markdown code fences.
- Preserve line breaks and paragraph structure in both sections.
- Transcribe the source in its original language, keeping its spelling and \
punctuation. Do not correct it.
- Give only the translation; do not mention which language it ended up in or why.
- If there is no readable text at all, write a single hyphen as the translation \
and leave the source section empty."
    )
}

/// A reply being streamed in.
///
/// The model answers with the translation first and the transcribed source
/// after the separator, so that the part the user is waiting for arrives first.
#[derive(Debug, Default)]
pub struct Reply {
    translation: String,
    source: String,
    past_separator: bool,
}

impl Reply {
    pub fn new() -> Self {
        Self::default()
    }

    /// Appends one streamed delta.
    pub fn push(&mut self, delta: &str) {
        let mut rest = delta;
        if !self.past_separator {
            let Some((before, after)) = rest.split_once(SEPARATOR) else {
                self.translation.push_str(rest);
                return;
            };
            self.translation.push_str(before);
            self.past_separator = true;
            rest = after;
        }
        self.source.push_str(rest);
    }

    /// The translation received so far.
    pub fn translation(&self) -> &str {
        self.translation.trim()
    }

    /// The transcribed source text, empty until the separator arrives.
    pub fn source(&self) -> &str {
        self.source.trim()
    }

    /// Whether the separator has been seen.
    pub fn has_source(&self) -> bool {
        self.past_separator
    }

    /// Whether anything usable arrived at all.
    pub fn is_empty(&self) -> bool {
        self.translation().is_empty() && self.source().is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn reply_of(deltas: &[&str]) -> Reply {
        let mut reply = Reply::new();
        for delta in deltas {
            reply.push(delta);
        }
        reply
    }

    #[test]
    fn collects_translation_before_the_separator() {
        let reply = reply_of(&["你好", "，", "世界"]);
        assert_eq!(reply.translation(), "你好，世界");
        assert_eq!(reply.source(), "");
        assert!(!reply.has_source());
    }

    #[test]
    fn splits_translation_from_source() {
        let reply = reply_of(&["你好\n⁂\nHello"]);
        assert_eq!(reply.translation(), "你好");
        assert_eq!(reply.source(), "Hello");
        assert!(reply.has_source());
    }

    #[test]
    fn splits_when_separator_arrives_in_its_own_delta() {
        let reply = reply_of(&["你好\n", "⁂", "\nHello"]);
        assert_eq!(reply.translation(), "你好");
        assert_eq!(reply.source(), "Hello");
    }

    #[test]
    fn splits_when_separator_is_glued_to_neighbouring_text() {
        let reply = reply_of(&["你好⁂Hel", "lo"]);
        assert_eq!(reply.translation(), "你好");
        assert_eq!(reply.source(), "Hello");
    }

    #[test]
    fn keeps_a_second_separator_as_source_text() {
        let reply = reply_of(&["你好\n⁂\nHello ⁂ world"]);
        assert_eq!(reply.translation(), "你好");
        assert_eq!(reply.source(), "Hello ⁂ world");
    }

    #[test]
    fn keeps_newlines_inside_each_part() {
        let reply = reply_of(&["第一行\n第二行\n⁂\nline one\nline two"]);
        assert_eq!(reply.translation(), "第一行\n第二行");
        assert_eq!(reply.source(), "line one\nline two");
    }

    #[test]
    fn tolerates_a_missing_source_section() {
        let reply = reply_of(&["你好"]);
        assert_eq!(reply.translation(), "你好");
        assert!(!reply.has_source());
        assert!(!reply.is_empty());
    }

    #[test]
    fn tolerates_an_empty_source_section() {
        let reply = reply_of(&["你好\n⁂\n"]);
        assert_eq!(reply.translation(), "你好");
        assert_eq!(reply.source(), "");
        assert!(reply.has_source());
    }

    #[test]
    fn ignores_empty_deltas() {
        let reply = reply_of(&["", "你好", "", "⁂", "", "Hello", ""]);
        assert_eq!(reply.translation(), "你好");
        assert_eq!(reply.source(), "Hello");
    }

    #[test]
    fn a_reply_with_only_whitespace_is_empty() {
        assert!(reply_of(&["  \n ", "\t"]).is_empty());
        assert!(Reply::new().is_empty());
    }
}

#[cfg(test)]
mod prompt_tests {
    use super::*;

    #[test]
    fn names_the_target_language_and_the_separator() {
        let prompt = system_prompt("Simplified Chinese", "");
        assert!(prompt.contains("Simplified Chinese"));
        assert!(prompt.contains(SEPARATOR));
    }

    #[test]
    fn without_a_fallback_the_source_is_repeated_unchanged() {
        let prompt = system_prompt("English", "");
        assert!(prompt.contains("repeat it unchanged"));
        assert!(!prompt.contains("instead"));
    }

    #[test]
    fn a_fallback_replaces_the_repeat_rule_rather_than_adding_to_it() {
        let prompt = system_prompt("Simplified Chinese", "English");
        assert!(prompt.contains("translate it into English instead"));
        assert!(
            !prompt.contains("repeat it unchanged"),
            "the two rules would contradict each other"
        );
    }

    #[test]
    fn a_blank_fallback_counts_as_none() {
        assert_eq!(
            system_prompt("English", "   "),
            system_prompt("English", "")
        );
    }
}
