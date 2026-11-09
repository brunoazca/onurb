# Brief for Instinct — Onurb integration

Sent to Instinct over WhatsApp (via wacli). This copy is kept in the repo as the reference; no numbers or addresses here.

---

Hi Instinct! I'm building **Onurb**, a voice watch (Waveshare ESP32-S3). When I press its button, a small server on my Mac (the "Onurb gateway") turns my speech into text and **sends it to you on WhatsApp from my own account**, starting with `[onurb]`. Your reply is turned back into speech and played on the watch. I'll speak **English or Brazilian Portuguese**.

Everything goes through you: context, integrations, everything. Onurb itself is only the voice terminal. Here's what I'd like:

**1. Answering `[onurb]` messages**
- Short and speakable: plain sentences, no markdown, lists, emoji or links. If something needs a link or a lot of detail, say "sent to your phone" and send it as a separate message.
- Reply in the language I used (EN or pt-BR).
- One message per request if possible. For anything slow, reply right away with a short acknowledgement ("starting it now") and send the result later. The watch picks late messages up on my next press.

**2. Safety rule for `[onurb]` messages**
Anyone holding the watch can speak into it. For `[onurb]` requests that **spend money, message other people, or merge/deploy code**, please ask me to confirm here on the phone (not by voice) before doing it.

**3. To-do list**
Keep a to-do list I can manage by voice: add, complete, list, "what's next". Ideally synced with Apple, preferably Reminders, or iCloud Notes if that's easier for you. What can you do there?

**4. Shopping list + Friday order**
Keep a shopping list I can add to by voice. Every Friday, prepare the order and send it to me for approval (items + total). Only place it after I approve.

**5. Agent orchestrator**
By voice: start coding agents in my herdr workspaces (through the tunnel you already use), tell me their status, and report when they finish. Never merge or deploy without my explicit confirmation on the phone.

**6. Mail and calendar**
Whatever you already cover. By voice I'd mostly ask "what's next today", "anything urgent", "move X to Y".

**7. Later, not now**
Passwords/bank access (read-only first) and meeting recordings.

**Questions for you**
1. Which of 1–6 can you already do, and what would you need from me for the rest?
2. Do you usually reply in one message or several? Is there a way to tell when you're done?
3. Can you handle voice notes from me, and send voice notes back?
4. For the to-do list: Apple Reminders or iCloud Notes, which works for you?
5. Which grocery service could you order from on Fridays?

Thanks!
