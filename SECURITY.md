# Security Policy

LibOSDP is deployed in production by a wide range of users. We take the
security of the library and its downstream consumers seriously and welcome
reports of any issue that may have security implications.

## Reporting a Vulnerability

If you believe you have found a vulnerability, please follow responsible
disclosure practices and email us at security@osdp.dev ([GPG][2]). We will
acknowledge your report within 3 business days.

Once a maintainer has confirmed the issue, we ask that reporters honour a
90-day embargo before disclosing it publicly. This window allows us to notify
subscribers of the security mailing list and gives downstream consumers time to
update their systems.

## Supported Versions

LibOSDP provides security and bug fixes for the latest two [releases][1], in
addition to the `master` branch.

| Version | Branch        | Supported          |
| ------- | ------------- | ------------------ |
| <= 2.0  | N/A           | :x:                |
| 3.1.x   | release_3.1   | :white_check_mark: |
| 3.2.x   | release_3.2   | :white_check_mark: |
| latest  | master        | :white_check_mark: |

## Security Mailing List

If you use LibOSDP in a product or any other production capacity, we encourage
you to join our private security mailing list. Subscribers are notified of
critical incidents — such as vulnerabilities and their fixes or workarounds —
before the details are made public. To request access, email security@osdp.dev.

We recommend subscribing with a dedicated, role-based address such as
security@your-company.com rather than an individual's mailbox, as this keeps the
list current as personnel change.

Membership is limited to verified production users. Writing to us from a company
email address is usually sufficient to establish this.

You may also monitor the [security advisories][3] page, though it is updated only
after an issue has been disclosed publicly.

[1]: https://github.com/osdp-dev/libosdp/releases
[2]: https://github.com/sidcha.gpg
[3]: https://github.com/osdp-dev/libosdp/security/advisories
